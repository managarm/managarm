#ifndef FRIGG_SUPPORT_HPP
#define FRIGG_SUPPORT_HPP

void friggBeginLog();
void friggEndLog();
void friggPrintCritical(char c);
void friggPrintCritical(const char *str);
void friggPanic() __attribute__ (( noreturn ));

#endif