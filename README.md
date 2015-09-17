# Sandstorm Raw API Example App

This is an example [Sandstorm](https://sandstorm.io) application which uses the raw [Cap'n Proto](https://capnproto.org)-based Sandstorm API to serve a web UI without an HTTP server. Most apps instead use `sandstorm-http-bridge` wrapping a traditional server, but this one does't.

## Why do that?

You might want to write a Sandstorm app using the raw API if:

* You want to be as efficient as possible. Raw API apps -- especially if written in C++ or Rust -- take drastically less RAM, start much faster, and produce much smaller distributable packages.
* You want to avoid legacy cruft. A traditional HTTP server is a huge, complicated piece of code much of which is not important for Sandstorm.
* You want to use a language that has poor HTTP support, but good Cap'n Proto RPC support, such as C++ or Rust.

That said, you should prefer `sandstorm-http-bridge` around a traditional HTTP server if:

* You are porting an existing HTTP-based app.
* You want to build on top of a standard HTTP server framework.
* You want to use a programming language that doesn't have good Cap'n Proto RPC support.

## Instructions

1. Get a Linux machine.
2. Install [Cap'n Proto](https://capnproto.org). You'll need to use the latest code from git, not a release version, because Sandstorm and Cap'n Proto are developed together and Sandstorm uses unreleased features from Cap'n Proto.
3. Install [Sandstorm](https://github.com/sandstorm-io/sandstorm) and make sure it is successfully running locally.
4. Clone this git repo.
5. CD to the repo and `make dev`.
6. Open your local Sandstorm. The app should be visible there.
7. Ctrl+C in the terminal running `make dev` to disconnect.

_Note: You can ignore the bogus warning about `getaddrinfo` and static linking -- the app will never actually call `getaddrinfo`. We statically link the binary so that there's no need to include shared libs in the package, making it smaller and simpler._

_Note: Due to an ongoing disagreement between [GCC's](https://gcc.gnu.org/bugzilla/show_bug.cgi?id=58074) and [Clang's](https://llvm.org/bugs/show_bug.cgi?id=23764) interpretations of the C++ ABI, it is important that you use the same compiler to compile this example program as you use to compile Cap'n Proto. By default both will use GCC. You can run `make CXX=clang++` to build with Clang instead._

## Building your own

You can use this code as a starting point for your own app.

1. Edit `sandstorm-pkgdef.capnp`, read the comments, and edit. At the very least you want to rename the app and set a new App ID.
2. Place your own client code (HTML, CSS, Javascript, etc.) in the `client` directory.
3. Have your client code use HTTP GET, PUT, and DELETE requests to store user data under the HTTP path `/var`. You can create files under `/var` with any name, but you cannot create sub-directories (for now).
4. Use `make dev` to test.
5. Type just `make` to build a distributable package `package.spk`.

Note that `server.c++` has some "features" that you may feel inclined to modify:

* Only the owner of an app instance is allowed to write (i.e. issue PUT or DELETE requests under `/var`). Anyone can read (though they must of course first receive the secret URL from the owner). For many apps, it's more appropriate for everyone to have write access (again, provided they've received the URL). Search for `canWrite` in the code, and `getViewInfo`.
* `Content-Type` headers are derived from file extensions, with only a small number of types supported. `index.html` is the default file for a directory. You can probably do better.
* Various useful Sandstorm API features are not exposed. You could implement an HTTP API to get access to those features from your client app.

## Questions?

[Ask on the dev group.](https://groups.google.com/group/sandstorm-dev)
