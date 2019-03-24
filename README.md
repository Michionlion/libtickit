# libtickit: Terminal Interface Construction Kit

[![Build Status](https://travis-ci.com/Michionlion/libtickit.svg?branch=master)](https://travis-ci.com/Michionlion/libtickit)

This library provides an abstracted mechanism for building interactive
full-screen terminal programs. It provides a full set of output drawing
functions, and handles keyboard and mouse input events.

## Overview

Using this library, applications can:

* Divide the terminal into a hierarchy of nested, possibly overlapping
  rectangular windows
* Render output content and react to input events independently in any window
  region
* Use fully Unicode-aware string content, including non-BMP, fullwidth and
  combining characters
* Draw line-art using Unicode box-drawing characters in a variety of styles
* Operate synchronously or asynchronously via file descriptors, or abstractly
  via byte buffers
* Recognise arbitrary keyboard input, including modifiers\*
* Make use of multiple terminals, if availble, from a single application

The following terminal features are supported:

* Many rendering attributes; bold, italics\*, underline, reverse,
  strikethough\*, alternate font\*
* 256 and 24-bit (16million) colors\*
* Mouse including mouse wheel and recognition of position reporting greater
  than 224 columns\*
* Arbitrary scrolling regions\*

\*: Not all terminals may support these features

### Project Management

The main project management site, containing bug lists, feature blueprints,
roadmaps, and a mirror of the source code is found on Launchpad, at
[launchpad.net/libtickit](https://launchpad.net/libtickit).

### Documentation

[Online versions](http://www.leonerd.org.uk/code/libtickit/doc/) of the
manpages are available in HTML form.

### Source

The primary upstream revision control system lives in [Bazaar](http://bazaar.leonerd.org.uk/c/libtickit).

This is mirrored by the Launchpad project above.

### Changes

There are some issues with testing in Travis CI, although locally everything
passes out; this is likely due to `terminfo` differences.

The main change in this fork is the addition of checking `$COLORTERM` for
setting `rgb8` capabilities. This only works for `xterm` terminals, and works
in conjunction with the palette testing -- I found that my local `xterm` did not
pass the palette test, but was capable of direct color. With these changes, now
`$COLORTERM` can be set to `truecolor` or `24bit`, and direct color will be
used.

There were also some bugs fixed. These commits contain fixes:

* ***[0c9fe2c](https://github.com/Michionlion/libtickit/commit/0c9fe2cdc832f6c5791ee1f1877918b0265a11db)***
  * When a `Tickit` is initialized it is not cleared, and therefore the `ti_hook`
  struct has invalid contents, since it is never set by default. This fix uses
  `calloc()` instead of `malloc()` to allocate the memory for a `Tickit`.
* ***[4cc85fb](https://github.com/Michionlion/libtickit/commit/4cc85fbcfb0a3ad82d87742866817490f479a802)***
  * A TickitTermBuilder's `ti_hook` was set after it was used to build a
  `TickitTermDriver`. This fix moves the setting to before
  `tickit_term_build_driver()` is called.
