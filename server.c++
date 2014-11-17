// Copyright (c) 2014 Sandstorm Development Group, Inc.
// Licensed under the MIT License:
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

// Hack around stdlib bug with C++14.
#include <initializer_list>  // force libstdc++ to include its config
#undef _GLIBCXX_HAVE_GETS    // correct broken config
// End hack.

#include <kj/main.h>
#include <kj/debug.h>
#include <kj/io.h>
#include <kj/async-io.h>
#include <capnp/rpc-twoparty.h>
#include <capnp/serialize.h>
#include <unistd.h>
#include <map>
#include <unordered_map>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <algorithm>

#include <sandstorm/grain.capnp.h>
#include <sandstorm/web-session.capnp.h>
#include <sandstorm/hack-session.capnp.h>

namespace {

#if __QTCREATOR
#define KJ_MVCAP(var) var
// QtCreator dosen't understand C++14 syntax yet.
#else
#define KJ_MVCAP(var) var = ::kj::mv(var)
// Capture the given variable by move.  Place this in a lambda capture list.  Requires C++14.
//
// TODO(cleanup):  Move to libkj.
#endif

typedef unsigned int uint;
typedef unsigned char byte;

// =======================================================================================
// Utility functions
//
// Most of these should be moved to the KJ library someday.

kj::AutoCloseFd createFile(kj::StringPtr name, int flags, mode_t mode = 0666) {
  // Create a file, returning an RAII wrapper around the file descriptor. Errors throw exceptinos.

  int fd;
  KJ_SYSCALL(fd = open(name.cStr(), O_CREAT | flags, mode), name);
  return kj::AutoCloseFd(fd);
}

kj::Maybe<kj::AutoCloseFd> tryOpen(kj::StringPtr name, int flags, mode_t mode = 0666) {
  // Try to open a file, returning an RAII wrapper around the file descriptor, or null if the
  // file doesn't exist. All other errors throw exceptions.

  int fd;

  while ((fd = open(name.cStr(), flags, mode)) < 0) {
    int error = errno;
    if (error == ENOENT) {
      return nullptr;
    } else if (error != EINTR) {
      KJ_FAIL_SYSCALL("open(name)", error, name);
    }
  }

  return kj::AutoCloseFd(fd);
}

bool isDirectory(kj::StringPtr filename) {
  // Return true if the parameter names a directory, false if it's any other kind of node or
  // doesn't exist.

  struct stat stats;
  while (stat(filename.cStr(), &stats) < 0) {
    if (errno != EINTR) {
      return false;
    }
  }
  return S_ISDIR(stats.st_mode);
}

kj::Vector<kj::String> listDirectory(kj::StringPtr dirname) {
  // Return a list of all filenames in the given directory, except "." and "..".

  kj::Vector<kj::String> entries;

  DIR* dir = opendir(dirname.cStr());
  if (dir == nullptr) {
    KJ_FAIL_SYSCALL("opendir", errno, dirname);
  }
  KJ_DEFER(closedir(dir));

  for (;;) {
    errno = 0;
    struct dirent* entry = readdir(dir);
    if (entry == nullptr) {
      int error = errno;
      if (error == 0) {
        break;
      } else {
        KJ_FAIL_SYSCALL("readdir", error, dirname);
      }
    }

    kj::StringPtr name = entry->d_name;
    if (name != "." && name != "..") {
      entries.add(kj::heapString(entry->d_name));
    }
  }

  return entries;
}

// =======================================================================================
// WebSession implementation (interface declared in sandstorm/web-session.capnp)

class UnauthorizedRequestStream final: public sandstorm::WebSession::RequestStream::Server {
  // A WebSession::RequestStream that always reports unauthorized.

public:
  // Ignore all input.
  kj::Promise<void> write(WriteContext context) override { return kj::READY_NOW; }
  kj::Promise<void> done(DoneContext context) override { return kj::READY_NOW; }
  kj::Promise<void> expectSize(ExpectSizeContext context) override { return kj::READY_NOW; }

  kj::Promise<void> getResponse(GetResponseContext context) override {
    // Immediately return error.
    context.getResults().initClientError()
        .setStatusCode(sandstorm::WebSession::Response::ClientErrorCode::FORBIDDEN);
    return kj::READY_NOW;
  }
};

class PutStreamImpl final: public sandstorm::WebSession::RequestStream::Server {
  // A WebSession::RequestStream that accepts a PUT request and writes it to disk.

public:
  PutStreamImpl(kj::AutoCloseFd fd, kj::String targetPath, kj::String tempPath)
      : fd(kj::mv(fd)), targetPath(kj::mv(targetPath)), tempPath(kj::mv(tempPath)),
        donePromiseAndFulfiller(kj::newPromiseAndFulfiller<void>()) {}

  kj::Promise<void> write(WriteContext context) override {
    KJ_REQUIRE(!isDone, "aleady called done()");

    sendExceptionsToGetResponse([&]() {
      kj::ArrayPtr<const byte> data = context.getParams().getData();

      // Write the data to disk.
      for (;;) {
        ssize_t n;
        KJ_SYSCALL(n = ::write(fd, data.begin(), data.size()));
        if (n == data.size()) {
          // All done.
          return;
        }
        data = data.slice(n, data.size());
      }
    });

    return kj::READY_NOW;
  }

  kj::Promise<void> done(DoneContext context) override {
    KJ_REQUIRE(!isDone, "aleady called done()");

    sendExceptionsToGetResponse([&]() {
      KJ_SYSCALL(rename(tempPath.cStr(), targetPath.cStr()));
      donePromiseAndFulfiller.fulfiller->fulfill();
    });

    return kj::READY_NOW;
  }

  kj::Promise<void> expectSize(ExpectSizeContext context) override {
    // Ignore; don't care.
    return kj::READY_NOW;
  }

  kj::Promise<void> getResponse(GetResponseContext context) override {
    context.releaseParams();
    context.getResults().initNoContent();

    // Wait for done() to be called or an error to occur, then return.
    return kj::mv(donePromiseAndFulfiller.promise);
  }

private:
  kj::AutoCloseFd fd;
  kj::String targetPath;
  kj::String tempPath;
  bool isDone = false;

  kj::PromiseFulfillerPair<void> donePromiseAndFulfiller;

  template <typename Func>
  void sendExceptionsToGetResponse(Func&& func) {
    // Try to run func(). If it throws an exception, cause getResponse() to fail with said
    // exception.

    KJ_IF_MAYBE(exception, kj::runCatchingExceptions(kj::fwd<Func>(func))) {
      donePromiseAndFulfiller.fulfiller->reject(kj::mv(*exception));
    }
  }
};

class StreamWriter final: public sandstorm::Handle::Server {
  // Object which reads data from a file on disk and writes it to a ByteStream. Used when streaming
  // a response to a GET request.

public:
  StreamWriter(kj::AutoCloseFd fd, sandstorm::ByteStream::Client stream)
      : fd(kj::mv(fd)), stream(kj::mv(stream)),
        writeLoop(nextWrite().eagerlyEvaluate([this](kj::Exception&& e) {
          // If we get here, the write loop failed. Alas, we have nowhere to report this failure.
          // This is a limitation of HTTP: if you've started writing a response and then something
          // fails, it's too late to go back and write an error response. All you can do is abort
          // the connection so that the client doesn't wait forever. The WebSession equivalent of
          // that is dropping the ByteStream, which we can do by overwriting it with null.
          this->stream = nullptr;
        })) {}

private:
  kj::AutoCloseFd fd;
  sandstorm::ByteStream::Client stream;

  kj::Promise<void> writeLoop;
  // The promise

  kj::Promise<void> nextWrite() {
    auto request = stream.writeRequest();

    // For maximum efficiency, we want to read directly into the request message. This means we
    // need to allocate space in the message before we start reading. But it might turn out that
    // we don't actually read as many bytes as we allocated. Normally Cap'n Proto doesn't allow
    // "un-allocating" space in a message, but makes a special exception for a text or data blob:
    // we can truncate the blob (and reclaim the space in the message) as long as we haven't
    // allocated any new space after allocating the blob. To use this facility, we need to allocate
    // the blob as an orphan.

    auto dataOrphan = capnp::Orphanage::getForMessageContaining(
          kj::implicitCast<sandstorm::ByteStream::WriteParams::Builder>(request))
        .newOrphan<capnp::Data>(8192);
    auto data = dataOrphan.get();

    ssize_t n;
    KJ_SYSCALL(n = ::read(fd, data.begin(), data.size()));

    if (n == 0) {
      // Oops, we're all done. Abandon that request and send a "done" request instead.
      return stream.doneRequest().send().then([](auto response) {});
    }

    if (n < data.size()) {
      // We read less than we requested, so truncate.
      dataOrphan.truncate(n);
    }

    // OK, now we can adopt the orphan into the message.
    request.adoptData(kj::mv(dataOrphan));

    return request.send().then([this](auto response) {
      return nextWrite();
    });
  }
};

class WebSessionImpl final: public sandstorm::WebSession::Server {
public:
  WebSessionImpl(sandstorm::UserInfo::Reader userInfo,
                 sandstorm::SessionContext::Client context,
                 sandstorm::WebSession::Params::Reader params) {
    // Permission #0 is "write". Check if bit 0 in the PermissionSet is set.
    auto permissions = userInfo.getPermissions();
    canWrite = permissions.size() > 0 && (permissions[0] & 1);

    // `UserInfo` is defined in `sandstorm/grain.capnp` and contains info like:
    // - A stable ID for the user, so you can correlate sessions from the same user.
    // - The user's display name, e.g. "Mark Miller", useful for identifying the user to other
    //   users.
    // - The user's permissions (seen above).

    // `WebSession::Params` is defined in `sandstorm/web-session.capnp` and contains info like:
    // - The hostname where the grain was mapped for this user. Every time a user opens a grain,
    //   it is mapped at a new random hostname for security reasons.
    // - The user's User-Agent and Accept-Languages headers.

    // `SessionContext` is defined in `sandstorm/grain.capnp` and implements callbacks for
    // sharing/access control and service publishing/discovery.
  }

  kj::Promise<void> get(GetContext context) override {
    // HTTP GET request.

    auto path = context.getParams().getPath();
    requireCanonicalPath(path);

    if (path == "var" || path == "var/") {
      // Return a listing of the directory contents, one per line.
      auto text = kj::strArray(listDirectory("var"), "\n");
      auto response = context.getResults().initContent();
      response.setMimeType("text/plain");
      response.getBody().setBytes(
          kj::arrayPtr(reinterpret_cast<byte*>(text.begin()), text.size()));
      return kj::READY_NOW;
    } else if (path.startsWith("var/")) {
      // Serve all files under /var with type application/octet-stream since it comes from the
      // user. E.g. serving as "text/html" here would allow someone to trivially XSS other users
      // of the grain by PUTing malicious HTML content. (Such an attack wouldn't be a huge deal:
      // it would only allow the attacker to hijack another user's access to this grain, not to
      // Sandstorm in general, and if they attacker already has write access to upload the
      // malicious content, they have little to gain from hijacking another session.)
      return readFile(path, context, "application/octet-stream");
    } else if (path == "" || path.endsWith("/")) {
      // A directory. Serve "index.html".
      return readFile(kj::str("client/", path, "index.html"), context, "text/html; charset=UTF-8");
    } else {
      // Request for a static file. Look for it under "client/".
      auto filename = kj::str("client/", path);

      // Check if it's a directory.
      if (isDirectory(filename)) {
        // It is. Return redirect to add '/'.
        auto redirect = context.getResults().initRedirect();
        redirect.setIsPermanent(true);
        redirect.setSwitchToGet(true);
        redirect.setLocation(kj::str(path, '/'));
        return kj::READY_NOW;
      }

      // Regular file (or non-existent).
      return readFile(kj::mv(filename), context, inferContentType(path));
    }
  }

  kj::Promise<void> putStreaming(PutStreamingContext context) override {
    // HTTP PUT request.

    auto path = context.getParams().getPath();
    requireCanonicalPath(path);

    KJ_REQUIRE(path.startsWith("var/"), "PUT only supported under /var.");

    if (!canWrite) {
      context.getResults().setStream(kj::heap<UnauthorizedRequestStream>());
    } else {
      auto tempPath = kj::str(path, ".uploading");
      auto fd = createFile(tempPath, O_WRONLY | O_TRUNC);
      context.getResults().setStream(
          kj::heap<PutStreamImpl>(kj::mv(fd), kj::heapString(path), kj::mv(tempPath)));
    }

    return kj::READY_NOW;
  }

  kj::Promise<void> delete_(DeleteContext context) override {
    // HTTP DELETE request.

    auto path = context.getParams().getPath();
    requireCanonicalPath(path);

    KJ_REQUIRE(path.startsWith("var/"), "DELETE only supported under /var.");

    if (!canWrite) {
      context.getResults().initClientError()
          .setStatusCode(sandstorm::WebSession::Response::ClientErrorCode::FORBIDDEN);
    } else {
      while (unlink(path.cStr()) != 0) {
        int error = errno;
        if (error == ENOENT) {
          // Ignore file-not-found for idempotency.
          break;
        } else if (error != EINTR) {
          KJ_FAIL_SYSCALL("unlink", error);
        }
      }
    }

    return kj::READY_NOW;
  }

private:
  bool canWrite;
  // True if the user has write permission.

  void requireCanonicalPath(kj::StringPtr path) {
    // Require that the path doesn't contain "." or ".." or consecutive slashes, to prevent path
    // injection attacks.
    //
    // Note that such attacks wouldn't actually accomplish much since everything outside /var
    // is a read-only filesystem anyway, containing the app package contents which are non-secret.

    KJ_REQUIRE(!path.startsWith("/"));
    KJ_REQUIRE(!path.startsWith("./") && path != ".");
    KJ_REQUIRE(!path.startsWith("../") && path != "..");

    KJ_IF_MAYBE(slashPos, path.findFirst('/')) {
      requireCanonicalPath(path.slice(*slashPos + 1));
    }
  }

  kj::StringPtr inferContentType(kj::StringPtr filename) {
    if (filename.endsWith(".html")) {
      return "text/html; charset=UTF-8";
    } else if (filename.endsWith(".js")) {
      return "text/javascript; charset=UTF-8";
    } else if (filename.endsWith(".css")) {
      return "text/css; charset=UTF-8";
    } else if (filename.endsWith(".png")) {
      return "image/png";
    } else if (filename.endsWith(".gif")) {
      return "image/gif";
    } else if (filename.endsWith(".jpg") || filename.endsWith(".jpeg")) {
      return "image/jpeg";
    } else if (filename.endsWith(".svg")) {
      return "image/svg+xml; charset=UTF-8";
    } else if (filename.endsWith(".txt")) {
      return "text/plain; charset=UTF-8";
    } else {
      return "application/octet-stream";
    }
  }

  kj::Promise<void> readFile(
      kj::StringPtr filename, GetContext context, kj::StringPtr contentType) {
    KJ_IF_MAYBE(fd, tryOpen(filename, O_RDONLY)) {
      auto response = context.getResults().initContent();
      response.setMimeType(contentType);
      response.getBody().setStream(kj::heap<StreamWriter>(
          kj::mv(*fd), context.getParams().getContext().getResponseStream()));
      return kj::READY_NOW;
    } else {
      auto error = context.getResults().initClientError();
      error.setStatusCode(sandstorm::WebSession::Response::ClientErrorCode::NOT_FOUND);
      return kj::READY_NOW;
    }
  }
};

// =======================================================================================
// UiView implementation (interface declared in sandstorm/grain.capnp)

class UiViewImpl final: public sandstorm::UiView::Server {
public:
  kj::Promise<void> getViewInfo(GetViewInfoContext context) override {
    auto viewInfo = context.initResults();

    // Define a "write" permission. People who don't have this will get read-only access.
    //
    // Currently, Sandstorm does not support assigning permissions to individuals. There are only
    // three distinguishable permission levels:
    // - The owner has all permissions.
    // - People who know the grain's secret URL (e.g. because the owner shared it with them) can
    //   open the grain but have no permissions.
    // - Everyone else cannot even open the grain.
    //
    // Thus, only the grain owner will get our "write" permission, but someday it may be possible
    // for the owner to assign varying permissions to individual people.
    auto perms = viewInfo.initPermissions(1);
    perms[0].setName("write");

    return kj::READY_NOW;
  }

  kj::Promise<void> newSession(NewSessionContext context) override {
    auto params = context.getParams();

    KJ_REQUIRE(params.getSessionType() == capnp::typeId<sandstorm::WebSession>(),
               "Unsupported session type.");

    context.getResults().setSession(
        kj::heap<WebSessionImpl>(params.getUserInfo(), params.getContext(),
                                 params.getSessionParams().getAs<sandstorm::WebSession::Params>()));

    return kj::READY_NOW;
  }
};

// =======================================================================================
// Program main

class ServerMain {
public:
  ServerMain(kj::ProcessContext& context): context(context), ioContext(kj::setupAsyncIo()) {}

  kj::MainFunc getMain() {
    return kj::MainBuilder(context, "Sandstorm Thin Server",
                           "Intended to be run as the root process of a Sandstorm app.")
        .callAfterParsing(KJ_BIND_METHOD(*this, run))
        .build();
  }

  kj::MainBuilder::Validity run() {
    // Set up RPC on file descriptor 3.
    auto stream = ioContext.lowLevelProvider->wrapSocketFd(3);
    capnp::TwoPartyVatNetwork network(*stream, capnp::rpc::twoparty::Side::CLIENT);
    auto rpcSystem = capnp::makeRpcServer(network, kj::heap<UiViewImpl>());

    // Get the SandstormApi default capability from the supervisor.
    // TODO(soon):  We don't use this, but for some reason the connection doesn't come up if we
    //   don't do this restore.  Cap'n Proto bug?  v8capnp bug?  Shell bug?
    {
      capnp::MallocMessageBuilder message;
      auto vatId = message.getRoot<capnp::rpc::twoparty::VatId>();
      vatId.setSide(capnp::rpc::twoparty::Side::SERVER);
      sandstorm::SandstormApi::Client api =
          rpcSystem.bootstrap(vatId).castAs<sandstorm::SandstormApi>();
    }

    kj::NEVER_DONE.wait(ioContext.waitScope);
  }

private:
  kj::ProcessContext& context;
  kj::AsyncIoContext ioContext;
};

}  // anonymous namespace

KJ_MAIN(ServerMain)
