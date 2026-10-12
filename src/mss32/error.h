#ifndef ERROR_H
#define ERROR_H

// Build a JSON error report and send it to the telemetry backend (waits at most a few seconds).
void error_sendErrorData(const char* message);

void error_init();
void error_patch();

#endif