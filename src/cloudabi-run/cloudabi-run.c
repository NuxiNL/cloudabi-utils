// Copyright (c) 2015 Nuxi, https://nuxi.nl/
//
// This file is distributed under a 2-clause BSD license.
// See the LICENSE file for details.

// cloudabi-run - execute CloudABI programs safely
//
// The cloudabi-run utility can execute CloudABI programs with an exact
// set of file descriptors. It reads a YAML configuration from stdin.
// This data is converted to argument data (argdata_t) that can be
// accessed from program_main().
//
// !fd, !file and !socket nodes in the YAML file are converted to file
// descriptor entries in the argument data, meaning they will be
// available within the CloudABI process.

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <netinet/in.h>

#include <argdata.h>
#include <errno.h>
#include <fcntl.h>
#ifndef O_EXEC
#define O_EXEC O_RDONLY
#endif
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <netdb.h>
#include <program.h>
#include <stdarg.h>
#include <stdio.h>
#ifndef __NetBSD__
#include <stdnoreturn.h>
#else
#define noreturn _Noreturn
#endif
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <yaml.h>

#include "../libemulator/emulate.h"
#include "../libemulator/posix.h"

#define TAG_PREFIX "tag:nuxi.nl,2015:cloudabi/"

static const argdata_t *parse_object(yaml_parser_t *parser);

// Obtains the next event from the YAML input stream.
static void get_event(yaml_parser_t *parser, yaml_event_t *event) {
  do {
    if (!yaml_parser_parse(parser, event)) {
      fprintf(stderr, "stdin:%zu:%zu: Parse error\n", parser->mark.line + 1,
              parser->mark.column + 1);
      exit(127);
    }
  } while (event->type == YAML_DOCUMENT_END_EVENT ||
           event->type == YAML_STREAM_END_EVENT);
}

// Terminates execution due to a parse error.
static noreturn void exit_parse_error(const yaml_event_t *event,
                                      const char *message, ...) {
  fprintf(stderr, "stdin:%zu:%zu: ", event->start_mark.line + 1,
          event->start_mark.column + 1);
  va_list ap;
  va_start(ap, message);
  vfprintf(stderr, message, ap);
  va_end(ap);
  fputc('\n', stderr);
  exit(127);
}

// Parses a boolean value.
static const argdata_t *parse_bool(yaml_event_t *event) {
  const char *value = (const char *)event->data.scalar.value;
  size_t length = event->data.scalar.length;

  // All valid 'true' keywords.
  for (const char *w = "y\0Y\0yes\0Yes\0YES\0true\0True\0TRUE\0on\0On\0ON\0";
       *w != '\0';) {
    size_t w_length = strlen(w);
    if (length == w_length && memcmp(value, w, w_length) == 0) {
      yaml_event_delete(event);
      return &argdata_true;
    }
    w += w_length + 1;
  }

  // All valid 'false' keywords.
  for (const char *w = "n\0N\0no\0No\0NO\0false\0False\0FALSE\0off\0Off\0OFF\0";
       *w != '\0';) {
    size_t w_length = strlen(w);
    if (length == w_length && memcmp(value, w, w_length) == 0) {
      yaml_event_delete(event);
      return &argdata_false;
    }
    w += w_length + 1;
  }

  return NULL;
}

// Parses a file descriptor number.
static const argdata_t *parse_fd(yaml_event_t *event) {
  // Extract file descriptor number from data.
  const char *value = (const char *)event->data.scalar.value;
  unsigned long fd;
  if (strcmp(value, "stdout") == 0) {
    fd = STDOUT_FILENO;
  } else if (strcmp(value, "stderr") == 0) {
    fd = STDERR_FILENO;
  } else {
    char *endptr;
    errno = 0;
    fd = strtoul(value, &endptr, 10);
    if (errno != 0 || endptr != value + event->data.scalar.length ||
        fd > INT_MAX)
      exit_parse_error(event, "Invalid file descriptor number: %s", value);
  }

  // Validate that this descriptor actually exists. program_exec() does
  // not return which file descriptors are invalid, so we'd better check
  // this manually over here.
  struct stat sb;
  if (fstat(fd, &sb) != 0)
    exit_parse_error(event, "File descriptor %d: %s", fd, strerror(errno));
  yaml_event_delete(event);
  return argdata_create_fd(fd);
}

// Parses a file, opens it and returns a file descriptor number.
static const argdata_t *parse_file(const yaml_event_t *event,
                                   yaml_parser_t *parser) {
  const char *path = NULL;
  for (;;) {
    // Fetch key name and value.
    const argdata_t *key = parse_object(parser);
    if (key == NULL)
      break;
    const char *keystr;
    int error = argdata_get_str_c(key, &keystr);
    if (error != 0)
      exit_parse_error(event, "Bad attribute: %s", strerror(error));
    const argdata_t *value = parse_object(parser);

    if (strcmp(keystr, "path") == 0) {
      // Pathname.
      error = argdata_get_str_c(value, &path);
      if (error != 0)
        exit_parse_error(event, "Bad path attribute: %s", strerror(error));
    } else {
      exit_parse_error(event, "Unknown file attribute: %s", keystr);
    }
  }

  // Open the file.
  // TODO(ed): Make mode and rights adjustable.
  if (path == NULL)
    exit_parse_error(event, "Missing path attribute");
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    exit_parse_error(event, "Failed to open \"%s\": %s", path, strerror(errno));
  return argdata_create_fd(fd);
}

// Parses a floating point value.
static const argdata_t *parse_float(yaml_event_t *event) {
  const char *value = (const char *)event->data.scalar.value;
  double fpvalue;
  if (strcmp(value, ".inf") == 0) {
    fpvalue = INFINITY;
  } else if (strcmp(value, "-.inf") == 0) {
    fpvalue = -INFINITY;
  } else if (strcmp(value, ".nan") == 0) {
    fpvalue = NAN;
  } else {
    const char *value_end = value + event->data.scalar.length;
    char *endptr;
    errno = 0;
    fpvalue = strtod(value, &endptr);
    if (errno != 0 || endptr != value_end)
      return NULL;
  }
  yaml_event_delete(event);
  return argdata_create_float(fpvalue);
}

// Parses an integer value.
static const argdata_t *parse_int(yaml_event_t *event) {
  const char *value = (const char *)event->data.scalar.value;
  const char *value_end = value + event->data.scalar.length;

  // Determine the base of the number.
  int base;
  if (value[0] == '0' && value[1] == 'o') {
    value += 2;
    base = 8;
  } else if (value[0] == '0' && value[1] == 'x') {
    value += 2;
    base = 16;
  } else {
    base = 10;
  }

  // Try signed integer conversion.
  {
    intmax_t intval;
    char *endptr;
    errno = 0;
    intval = strtoimax(value, &endptr, base);
    if (errno == 0 && endptr == value_end) {
      yaml_event_delete(event);
      return argdata_create_int(intval);
    }
  }

  // Try unsigned integer conversion.
  {
    uintmax_t uintval;
    char *endptr;
    errno = 0;
    uintval = strtoumax(value, &endptr, base);
    if (errno == 0 && endptr == value_end) {
      yaml_event_delete(event);
      return argdata_create_int(uintval);
    }
  }

  return NULL;
}

// Parses a string value.
static const argdata_t *parse_str(const yaml_event_t *event) {
  return argdata_create_str((const char *)event->data.scalar.value,
                            event->data.scalar.length);
}

// Parses the next Base64 character from an input string and returns
// its value.
static unsigned char parse_binary_getchar(const char **value) {
  for (;;) {
    char c = *(*value)++;
    if (c >= 'A' && c <= 'Z')
      return c - 'A';
    if (c >= 'a' && c <= 'z')
      return c - 'a' + 26;
    if (c >= '0' && c <= '9')
      return c - '0' + 52;
    if (c == '+')
      return 62;
    if (c == '/')
      return 63;
  }
}

// Parses a binary string value.
static const argdata_t *parse_binary(yaml_event_t *event) {
  const char *value = (const char *)event->data.scalar.value;
  const char *value_end = value + event->data.scalar.length;

  // Count the number of Base64 characters in the input.
  size_t chars = 0;
  for (const char *c = value; c < value_end; ++c) {
    if ((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') ||
        (*c >= '0' && *c <= '9') || *c == '+' || *c == '/') {
      // Valid Base64 character.
      ++chars;
    } else if (*c != '=' && *c == '\n' && *c == ' ' && *c == '\t') {
      exit_parse_error(event, "Invalid character in Base64 input: %hhx", *c);
    }
  }

  // Allocate a buffer for storing the Base64 decoded data.
  size_t bytes = chars * 6 / 8;
  unsigned char *buf = malloc(bytes);
  if (buf == NULL) {
    perror("Cannot allocate binary data buffer");
    exit(127);
  }

  // Decode Base64 in groups of four characters into three bytes.
  unsigned char *p = buf;
  for (size_t i = 0; i < bytes / 3; ++i) {
    unsigned char c1 = parse_binary_getchar(&value);
    unsigned char c2 = parse_binary_getchar(&value);
    unsigned char c3 = parse_binary_getchar(&value);
    unsigned char c4 = parse_binary_getchar(&value);
    *p++ = c1 << 2 | c2 >> 4;
    *p++ = c2 << 4 | c3 >> 2;
    *p++ = c3 << 6 | c4;
  }

  // Decode the remainder.
  if (bytes % 3 == 1) {
    unsigned char c1 = parse_binary_getchar(&value);
    unsigned char c2 = parse_binary_getchar(&value);
    *p++ = c1 << 2 | c2 >> 4;
  } else if (bytes % 3 == 2) {
    unsigned char c1 = parse_binary_getchar(&value);
    unsigned char c2 = parse_binary_getchar(&value);
    unsigned char c3 = parse_binary_getchar(&value);
    *p++ = c1 << 2 | c2 >> 4;
    *p++ = c2 << 4 | c3 >> 2;
  }

  yaml_event_delete(event);
  return argdata_create_binary(buf, bytes);
}

// Parses a map.
static const argdata_t *parse_map(yaml_parser_t *parser) {
  const argdata_t **keys = NULL, **values = NULL;
  size_t nentries = 0, space = 0;
  for (;;) {
    const argdata_t *ad = parse_object(parser);
    if (ad == NULL)
      break;
    if (nentries == space) {
      space = space < 8 ? 8 : space * 2;
      keys = realloc(keys, space * sizeof(keys[0]));
      values = realloc(values, space * sizeof(values[0]));
    }
    keys[nentries] = ad;
    values[nentries++] = parse_object(parser);
  }
  return argdata_create_map(keys, values, nentries);
}

// Parses a null value.
static const argdata_t *parse_null(yaml_event_t *event) {
  yaml_event_delete(event);
  return &argdata_null;
}

// Parses a number at the beginning of a string for timestamp parsing.
static int parse_timestamp_number(const char **value, int min, int max) {
  int result = 0;
  for (int i = 0; i < max; ++i) {
    char c = **value;
    if (c < '0' || c > '9') {
      if (i < min)
        return -1;
      break;
    }
    result = result * 10 + *(*value)++ - '0';
  }
  return result;
}

// Parses a literal character for timestamp parsing.
static bool parse_timestamp_char(const char **value, char c) {
  if (**value == c) {
    ++*value;
    return true;
  }
  return false;
}

// Parses a timestamp value.
static const argdata_t *parse_timestamp(yaml_event_t *event) {
  const char *value = (const char *)event->data.scalar.value;
  const char *value_end = value + event->data.scalar.length;

  // Parse the date.
  struct tm tm = {};
  if ((tm.tm_year = parse_timestamp_number(&value, 4, 4)) < 0 ||
      !parse_timestamp_char(&value, '-') ||
      (tm.tm_mon = parse_timestamp_number(&value, 1, 2)) < 0 ||
      !parse_timestamp_char(&value, '-') ||
      (tm.tm_mday = parse_timestamp_number(&value, 1, 2)) < 0)
    return NULL;
  tm.tm_year -= 1900;
  --tm.tm_mon;

  struct timespec ts = {};
  if (value != value_end || event->data.scalar.length != 10) {
    // Parse the time separator.
    if (!parse_timestamp_char(&value, 't') &&
        !parse_timestamp_char(&value, 'T')) {
      bool gotspace = false;
      while (parse_timestamp_char(&value, ' ') ||
             parse_timestamp_char(&value, '\t'))
        gotspace = true;
      if (!gotspace)
        return NULL;
    }

    // Parse the time.
    if ((tm.tm_hour = parse_timestamp_number(&value, 1, 2)) < 0 ||
        !parse_timestamp_char(&value, ':') ||
        (tm.tm_min = parse_timestamp_number(&value, 2, 2)) < 0 ||
        !parse_timestamp_char(&value, ':') ||
        (tm.tm_sec = parse_timestamp_number(&value, 2, 2)) < 0)
      return NULL;

    // Parse fractional seconds.
    if (parse_timestamp_char(&value, '.')) {
      unsigned long multiplier = 100000000;
      while (*value >= '0' && *value <= '9') {
        ts.tv_nsec += (*value++ - '0') * multiplier;
        multiplier /= 10;
      }
    }

    // Allow whitespace between the time and the time zone.
    while (parse_timestamp_char(&value, ' ') ||
           parse_timestamp_char(&value, '\t'))
      ;

    // Parse the time zone offset.
    bool tz_negative;
    if ((tz_negative = parse_timestamp_char(&value, '-')) ||
        parse_timestamp_char(&value, '+')) {
      int tz_hour, tz_min = 0;
      if ((tz_hour = parse_timestamp_number(&value, 1, 2)) < 0 ||
          (parse_timestamp_char(&value, ':') &&
           (tz_min = parse_timestamp_number(&value, 2, 2)) < 0))
        return NULL;
      if (tz_negative) {
        tm.tm_hour += tz_hour;
        tm.tm_min += tz_min;
      } else {
        tm.tm_hour -= tz_hour;
        tm.tm_min -= tz_min;
      }
    } else {
      parse_timestamp_char(&value, 'Z');
    }

    // Disallow trailing garbage.
    if (value != value_end)
      return NULL;
  }

  ts.tv_sec = timegm(&tm);
  yaml_event_delete(event);
  return argdata_create_timestamp(&ts);
}

// Parses an unquoted string that doesn't have a tag associated with it.
// This means that we'll need to infer its type.
static const argdata_t *parse_str_autodetect(yaml_event_t *event) {
  // Potential null value?
  const char *value = (const char *)event->data.scalar.value;
  if (strcmp(value, "null") == 0) {
    yaml_event_delete(event);
    return &argdata_null;
  }

  // Potential boolean value?
  const argdata_t *ret = parse_bool(event);
  if (ret != NULL)
    return ret;

  // Potential integer value?
  ret = parse_int(event);
  if (ret != NULL)
    return ret;

  // Potential floating point value?
  ret = parse_float(event);
  if (ret != NULL)
    return ret;

  // Potential timestamp value?
  ret = parse_timestamp(event);
  if (ret != NULL)
    return ret;

  // Fall back to interpreting it as a plain string value.
  return parse_str(event);
}

// Parses a sequence.
static const argdata_t *parse_seq(yaml_parser_t *parser) {
  const argdata_t **entries = NULL;
  size_t nentries = 0, space = 0;
  for (;;) {
    const argdata_t *ad = parse_object(parser);
    if (ad == NULL)
      break;
    if (nentries == space) {
      space = space < 8 ? 8 : space * 2;
      entries = realloc(entries, space * sizeof(entries[0]));
    }
    entries[nentries++] = ad;
  }
  return argdata_create_seq(entries, nentries);
}

// Parses a socket, creates it and returns a file descriptor number.
// TODO(ed): Add support for connecting sockets.
static const argdata_t *parse_socket(const yaml_event_t *event,
                                     yaml_parser_t *parser) {
  const char *typestr = "stream", *bindstr = NULL;
  for (;;) {
    // Fetch key name and value.
    const argdata_t *key = parse_object(parser);
    if (key == NULL)
      break;
    const char *keystr;
    int error = argdata_get_str_c(key, &keystr);
    if (error != 0)
      exit_parse_error(event, "Bad attribute: %s", strerror(error));
    const argdata_t *value = parse_object(parser);

    if (strcmp(keystr, "type") == 0) {
      // Socket type: stream, datagram, etc.
      error = argdata_get_str_c(value, &typestr);
      if (error != 0)
        exit_parse_error(event, "Bad type attribute: %s", strerror(error));
    } else if (strcmp(keystr, "bind") == 0) {
      // Address on which to bind.
      error = argdata_get_str_c(value, &bindstr);
      if (error != 0)
        exit_parse_error(event, "Bad bind attribute: %s", strerror(error));
    } else {
      exit_parse_error(event, "Unknown socket attribute: %s", keystr);
    }
  }

  // Parse the socket type.
  int type;
  if (strcmp(typestr, "dgram") == 0)
    type = SOCK_DGRAM;
  else if (strcmp(typestr, "seqpacket") == 0)
    type = SOCK_SEQPACKET;
  else if (strcmp(typestr, "stream") == 0)
    type = SOCK_STREAM;
  else
    exit_parse_error(event, "Unsupported type attribute: %s", typestr);

  // Parse the bind address.
  if (bindstr == NULL)
    exit_parse_error(event, "Missing bind attribute");
  const struct sockaddr *sa;
  socklen_t sal;
  struct sockaddr_un sun;
  struct addrinfo *res = NULL;
  if (bindstr[0] == '/') {
    // UNIX socket: bind to path.
    sun.sun_family = AF_UNIX;
    strncpy(sun.sun_path, bindstr, sizeof(sun.sun_path));
    if (sun.sun_path[sizeof(sun.sun_path) - 1] != '\0')
      exit_parse_error(event, "Socket path %s too long", bindstr);
    sa = (const struct sockaddr *)&sun;
    sal = sizeof(sun);
  } else {
    // IPv4 or IPv6 socket. Extract address and port number.
    const char *hostname_start, *hostname_end, *servname;
    if (bindstr[0] == '[') {
      hostname_start = bindstr + 1;
      hostname_end = strstr(hostname_start, "]:");
      servname = hostname_end + 2;
    } else {
      hostname_start = bindstr;
      hostname_end = strchr(hostname_start, ':');
      servname = hostname_end + 1;
    }
    if (hostname_end == NULL)
      exit_parse_error(event, "Address %s does not contain a port number",
                       bindstr);

    // Resolve address and port number.
    char *hostname = strndup(hostname_start, hostname_end - hostname_start);
    struct addrinfo hint = {.ai_family = AF_UNSPEC, .ai_socktype = type};
    int error = getaddrinfo(hostname, servname, &hint, &res);
    free(hostname);
    if (error != 0)
      exit_parse_error(event, "Failed to resolve %s: %s", bindstr,
                       gai_strerror(error));
    if (res->ai_next != NULL)
      exit_parse_error(event, "%s resolves to multiple addresses", bindstr);
    sa = res->ai_addr;
    sal = res->ai_addrlen;
  }

  // Create socket.
  int fd = socket(sa->sa_family, type, 0);
  if (fd == -1)
    exit_parse_error(event, "Failed to create socket for %s: %s", bindstr,
                     strerror(errno));

  // Bind.
  int on = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  if (bind(fd, sa, sal) == -1)
    exit_parse_error(event, "Failed to bind to %s: %s", bindstr,
                     strerror(errno));
  if (listen(fd, 0) == -1 && errno != EOPNOTSUPP)
    exit_parse_error(event, "Failed to listen on %s: %s", bindstr,
                     strerror(errno));

  if (res != NULL)
    freeaddrinfo(res);
  return argdata_create_fd(fd);
}

static const argdata_t *parse_object(yaml_parser_t *parser) {
  yaml_event_t event;
  get_event(parser, &event);
  const char *tag = (const char *)event.data.scalar.tag;
  switch (event.type) {
    case YAML_STREAM_START_EVENT:
      yaml_event_delete(&event);
      return parse_object(parser);
    case YAML_DOCUMENT_START_EVENT:
      yaml_event_delete(&event);
      return parse_object(parser);
    case YAML_MAPPING_START_EVENT:
      if (tag == NULL || strcmp(tag, YAML_MAP_TAG) == 0) {
        return parse_map(parser);
      } else if (strcmp(tag, TAG_PREFIX "file") == 0) {
        return parse_file(&event, parser);
      } else if (strcmp(tag, TAG_PREFIX "socket") == 0) {
        return parse_socket(&event, parser);
      } else {
        exit_parse_error(&event, "Unsupported tag for mapping: %s", tag);
      }
      yaml_event_delete(&event);
    case YAML_SCALAR_EVENT:
      if (event.data.scalar.plain_implicit) {
        return parse_str_autodetect(&event);
      } else if (tag == NULL || strcmp(tag, YAML_STR_TAG) == 0) {
        return parse_str(&event);
      } else if (strcmp(tag, "tag:yaml.org,2002:binary") == 0) {
        return parse_binary(&event);
      } else if (strcmp(tag, YAML_BOOL_TAG) == 0) {
        const argdata_t *ret = parse_bool(&event);
        if (ret == NULL)
          exit_parse_error(&event, "Invalid boolean value: %s",
                           (const char *)event.data.scalar.value);
        return ret;
      } else if (strcmp(tag, YAML_FLOAT_TAG) == 0) {
        const argdata_t *ret = parse_float(&event);
        if (ret == NULL)
          exit_parse_error(&event, "Invalid floating point value: %s",
                           (const char *)event.data.scalar.value);
        return ret;
      } else if (strcmp(tag, YAML_INT_TAG) == 0) {
        const argdata_t *ret = parse_int(&event);
        if (ret == NULL)
          exit_parse_error(&event, "Invalid integer value: %s",
                           (const char *)event.data.scalar.value);
        return ret;
      } else if (strcmp(tag, YAML_NULL_TAG) == 0) {
        return parse_null(&event);
      } else if (strcmp(tag, "tag:yaml.org,2002:timestamp") == 0) {
        const argdata_t *ret = parse_timestamp(&event);
        if (ret == NULL)
          exit_parse_error(&event, "Invalid timestamp value: %s",
                           (const char *)event.data.scalar.value);
        return ret;
      } else if (strcmp(tag, TAG_PREFIX "fd") == 0) {
        return parse_fd(&event);
      } else {
        exit_parse_error(&event, "Unsupported tag for scalar: %s", tag);
      }
    case YAML_SEQUENCE_START_EVENT:
      if (tag == NULL || strcmp(tag, YAML_SEQ_TAG) == 0) {
        return parse_seq(parser);
      } else {
        exit_parse_error(&event, "Unsupported tag for sequence: %s", tag);
      }
      yaml_event_delete(&event);
    case YAML_MAPPING_END_EVENT:
    case YAML_SEQUENCE_END_EVENT:
      yaml_event_delete(&event);
      return NULL;
    default:
      exit_parse_error(&event, "Unsupported event %d", event.type);
  }
}

static noreturn void usage(void) {
  fprintf(stderr, "usage: cloudabi-run [-e] executable\n");
  exit(127);
}

int main(int argc, char *argv[]) {
  // Parse command line options.
  bool do_emulate = false;
  char c;
  while ((c = getopt(argc, argv, "e")) != -1) {
    switch (c) {
      case 'e':
        // Run program using emulation.
        do_emulate = true;
        break;
      default:
        usage();
    }
  }
  argv += optind;
  argc -= optind;
  if (argc != 1)
    usage();

  // Parse YAML configuration.
  yaml_parser_t parser;
  yaml_parser_initialize(&parser);
  yaml_parser_set_input_file(&parser, stdin);
  const argdata_t *ad = parse_object(&parser);
  yaml_parser_delete(&parser);

  if (do_emulate) {
    // Serialize argument data that needs to be passed to the executable.
    size_t buflen, fdslen;
    argdata_get_buffer_length(ad, &buflen, &fdslen);
    int *fds = malloc(fdslen * sizeof(fds[0]) + buflen);
    if (fds == NULL) {
      perror("Cannot allocate argument data buffer");
      exit(127);
    }
    void *buf = &fds[fdslen];
    fdslen = argdata_get_buffer(ad, buf, fds);

    // Register file descriptors.
    struct fd_table ft;
    fd_table_init(&ft);
    for (size_t i = 0; i < fdslen; ++i) {
      if (!fd_table_insert_existing(&ft, i, fds[i])) {
        perror("Failed to register file descriptor in argument data");
        exit(127);
      }
    }

    // Call into the emulator to run the program inside of this process.
    // Throw a warning message before beginning execution, as emulation
    // is not considered secure.
    int fd = open(argv[0], O_RDONLY);
    if (fd == -1) {
      perror("Failed to open executable");
      return 127;
    }
    fprintf(stderr,
            "WARNING: Attempting to start executable using emulation.\n"
            "Keep in mind that this emulation provides no actual sandboxing.\n"
            "Though this is likely no problem for development and testing\n"
            "purposes, using this emulator in production is strongly\n"
            "discouraged.\n");

    emulate(fd, buf, buflen, &posix_syscalls);
  } else {
    // Execute the application directly through the operating system.
    int fd = open(argv[0], O_EXEC);
    if (fd == -1) {
      perror("Failed to open executable");
      return 127;
    }
    errno = program_exec(fd, ad);
  }
  perror("Failed to start executable");
  return 127;
}
