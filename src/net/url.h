#ifndef DAUKLE_URL_H
#define DAUKLE_URL_H

/* Whether two urls share an origin: scheme, host and port, with the port
   defaulted from the scheme so "https://h/" and "https://h:443/" match.
   This is what decides whether a caller's headers survive a redirect, and it
   lives here rather than in each http backend because one rule implemented
   twice drifts: the two once disagreed about whether the scheme counted, and
   an https to http redirect re-sent an Authorization header in cleartext.
   A url that cannot be parsed compares unequal, so the headers are dropped. */
int fr_url_same_origin(const char *left, const char *right);

#endif
