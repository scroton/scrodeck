# Security

scroDeck runs terminals and Claude Code sessions on your machine and can serve them to a phone or another PC. A
problem that lets someone else reach those terminals, read a token, or run something on your machine matters.

**To report a problem:** open a private security advisory on this repository (the "Security" tab, "Report a
vulnerability"), so it stays private until it is fixed. Please do not open a public issue for it. Say what you saw,
how to reproduce it, and the version (`core/deck/version.hpp`).

What the deck does by design, to judge a report against:
- The phone / remote-desk server is off by default. When on, it listens on the Tailscale interface (or localhost
  only) and every request needs the token in the link. It is not meant to face the internet (`tailscale funnel`,
  port forwarding).
- The deck never writes your Claude Code credentials. It reads a sign-in token only for the opt-in live usage bars
  and the cloud-session list.
- A page shown through the deck's viewer (`/view/`) is served sandboxed, from a folder the deck registered, never
  from a path taken from the request.
