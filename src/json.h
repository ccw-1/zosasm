#ifndef ZA_JSON_H
#define ZA_JSON_H
#include <stddef.h>
#include "util.h"
typedef enum { JSON_NULL, JSON_BOOL, JSON_NUM, JSON_STR, JSON_ARR, JSON_OBJ } JsonType;
typedef struct Json Json;
struct Json {
    JsonType type;
    int b;
    double num;
    char *str;
    size_t n;
    Json **v;
    char **k;
};
Json *json_parse(const char *s, size_t n, const char **err);
void json_free(Json *j);
Json *json_get(const Json *o, const char *key);
const char *json_str(const Json *o, const char *key);
long json_int(const Json *o, const char *key, long def);
int json_bool(const Json *o, const char *key, int def);
void json_put_str(Buf *b, const char *s, size_t n);
void json_put_cstr(Buf *b, const char *s);
#endif
