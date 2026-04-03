#ifndef FASTWSGI_REQUEST_H_
#define FASTWSGI_REQUEST_H_

#include "common.h"
#include "llhttp.h"
#include "start_response.h"


void init_request_def_env();
void configure_parser_settings(llhttp_settings_t * ps);
void close_iterator(PyObject * iterator);

#endif
