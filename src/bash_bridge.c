#include "config.h"

#include "builtins.h"
#include "variables.h"

#include <stdio.h>
#include <string.h>

extern int proto_bash_enable(const char *binary);
extern void proto_bash_disable(void);
extern int proto_bash_active(void);
extern int proto_bash_print_settings(void);

char **proto_bash_environment(void) {
    maybe_make_export_env();
    return export_env;
}

const char *proto_bash_variable(const char *name) {
    return get_string_value(name);
}

static int proto_hook_builtin(WORD_LIST *arguments) {
    if (arguments == NULL) {
        puts(proto_bash_active() ? "proto: Bash integration enabled"
                                 : "proto: Bash integration disabled");
        return 0;
    }
    const char *action = arguments->word->word;
    if (strcmp(action, "off") == 0 && arguments->next == NULL) {
        proto_bash_disable();
        return 0;
    }
    if (strcmp(action, "settings") == 0 && arguments->next == NULL) {
        return proto_bash_print_settings();
    }
    if (strcmp(action, "on") == 0 && arguments->next != NULL && arguments->next->next == NULL) {
        return proto_bash_enable(arguments->next->word->word);
    }
    fputs("usage: proto_hook on /absolute/path/to/proto | off | settings\n", stderr);
    return 2;
}

void proto_hook_builtin_unload(char *name) {
    (void)name;
    proto_bash_disable();
}

static char *documentation[] = {"Enable or disable proto's Readline hooks.", NULL};

struct builtin proto_hook_struct = {"proto_hook",
                                    proto_hook_builtin,
                                    BUILTIN_ENABLED,
                                    documentation,
                                    "proto_hook on /absolute/path/to/proto | off | settings",
                                    NULL};
