# CHANGELOG

## 2019-03-17 22:11  0.3

* Renamed:
  * tickit_timer_after_msec => tickit_watch_timer_after_msec
  * tickit_timer_after_tv   => tickit_watch_timer_after_tv
  * tickit_later            => tickit_watch_later
  * tickit_timer_cancel     => tickit_watch_cancel
* Added toplevel Tickit functions for loop control
  * tickit_tick(3)
  * tickit_watch_io_read(3)
  * tickit_watch_timer_at_epoch(3)
  * tickit_watch_timer_at_tv(3)
* Added TickitType
  * tickit_term_ctltype(3)
* Added window controls
  * tickit_window_getctl_int(3)
  * tickit_window_setctl_int(3)
  * tickit_window_ctlname(3)
  * tickit_window_lookup_ctl(3)
  * tickit_window_ctltype(3)
* Added toplevel Tickit instance controls
  * tickit_getctl_int(3)
  * tickit_setctl_int(3)
  * tickit_ctlname(3)
  * tickit_lookup_ctl(3)
  * tickit_ctltype(3)
* TICKIT_PEN_UNDER type is now integer, supports single, double, wavy underline
* Fixed various example/demo-\*.c files so they work again
* Added experimental new ability to provide event loop hooks for other event
  loops
* Deleted the deprecated tickit_string_\* functions that were renamed to
  tickit_utf8_\*
* Renumbered TICKIT_PEN_\* constants to start from 1; lists can end with 0
* Use termios to know what backspace (VERASE) character is, rather than
  relying on terminfo - experience from vim, neovim, etc.. is that this is
  often more reliable
* tickit_watch_\* callbacks now receive an info pointer, to match the calling
  style of other object bindings

## 2018-01-05 14:47  0.2

* Added tickit_window_{is,set}_steal_focus(3)
* Added entire toplevel Tickit instance
* New bind_events API style - one event + flags, instead of bitmask
* Renamed:
  * tickit_string_seqlen   => tickit_utf8_seqlen
  * tickit_string_putchar  => tickit_utf8_put
  * tickit_string_mbswidth => tickit_utf8_mbswidth
  * tickit_string_byte2col => tickit_utf8_byte2col
  * tickit_string_col2byte => tickit_utf8_col2byte
  * tickit_string_count\*   => tickit_utf8_count\*
* Added TickitString
* Added tickit_rectset_get_rect(3)
* Added tickit_renderbuffer_skiprect(3)
* Added tickit_renderbuffer_moverect(3)
* Added tickit_term_ctlname(3) and tickit_term_lookup_ctl(3)
* Added tickit_term_pause(3), tickit_term_resume(3)
* Added secondary RGB8 pen attributes
* Support RGB8 in xterm driver
