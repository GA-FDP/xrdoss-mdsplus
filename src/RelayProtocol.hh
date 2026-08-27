#ifndef FDP_RELAYPROTOCOL_HH
#define FDP_RELAYPROTOCOL_HH

// The wire contract between the relay (HttpRelay.cc, server side) and the
// transport (HttpTunnel.cc, client side).
//
// These lived as separate literals in the two files until the client had to
// recognise one of them -- a session the relay has retired -- and act on it.
// A string the client matches on and the server emits is an interface, and an
// interface spelled out twice drifts silently: the client would simply stop
// recovering, with no failing test and no log line saying why.

namespace fdp {

// Names the session on every call after /connect.
const char *const kSessionHeader = "x-fdp-session";

// The 502 body sent when a call names a session the relay no longer holds.
//
// The relay retires a session whenever a call fails -- a half-written call or
// an unread answer leaves the mdsip byte stream unresynchronisable, so the
// connection cannot be handed to the next caller. The client cannot tell that
// from the session having been idle-reaped, and does not need to: either way
// the fix is the same, dial a new session. Matching this exact body is what
// separates "your session is gone, retry" from every other 502.
const char *const kSessionGone = "unknown or expired session";

}  // namespace fdp

#endif
