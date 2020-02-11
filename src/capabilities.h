#ifndef __CAPABILITIES_H__
#define __CAPABILITIES_H__

typedef struct {
  char *representation;
  char *uuid;
  char *adminurl;
  char *duplex;
  char *is;
  char *cs;
  char *pdl;
  char *ty;
  char *vers;
} ippScanner;

typedef struct {
  char *representation;
  char *uuid;
  char *adminurl;
} ippPrinter;

int is_scanner_present(ippScanner *scanner, int port);
ippScanner *free_scanner(ippScanner *scanner);
int ipp_request(ippPrinter *printer, int port);
ippPrinter *free_printer(ippPrinter *printer);

#endif