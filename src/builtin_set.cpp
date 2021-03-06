// Functions used for implementing the set builtin.
#include "config.h"  // IWYU pragma: keep

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#include <algorithm>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "builtin.h"
#include "common.h"
#include "env.h"
#include "expand.h"
#include "fallback.h"  // IWYU pragma: keep
#include "io.h"
#include "proc.h"
#include "wgetopt.h"
#include "wutil.h"  // IWYU pragma: keep

class parser_t;

// Error message for invalid path operations.
#define BUILTIN_SET_PATH_ERROR L"%ls: Warning: $%ls entry \"%ls\" is not valid (%s)\n"

// Hint for invalid path operation with a colon.
#define BUILTIN_SET_PATH_HINT L"%ls: Did you mean 'set %ls $%ls %ls'?\n"

// Error for mismatch between index count and elements.
#define BUILTIN_SET_ARG_COUNT \
    L"%ls: The number of variable indexes does not match the number of values\n"

// Test if the specified variable should be subject to path validation.
static const wcstring_list_t path_variables({L"PATH", L"CDPATH"});
static int is_path_variable(const wchar_t *env) { return contains(path_variables, env); }

/// Call env_set. If this is a path variable, e.g. PATH, validate the elements. On error, print a
/// description of the problem to stderr.
static int my_env_set(const wchar_t *key, const wcstring_list_t &list, int scope,
                      io_streams_t &streams) {
    if (is_path_variable(key)) {
        // Fix for https://github.com/fish-shell/fish-shell/issues/199 . Return success if any path
        // setting succeeds.
        bool any_success = false;

        // Don't bother validating (or complaining about) values that are already present. When
        // determining already-present values, use ENV_DEFAULT instead of the passed-in scope
        // because in:
        //
        //   set -l PATH stuff $PATH
        //
        // where we are temporarily shadowing a variable, we want to compare against the shadowed
        // value, not the (missing) local value. Also don't bother to complain about relative paths,
        // which don't start with /.
        wcstring_list_t existing_values;
        const env_var_t existing_variable = env_get_string(key, ENV_DEFAULT);
        if (!existing_variable.missing_or_empty())
            tokenize_variable_array(existing_variable, existing_values);

        for (size_t i = 0; i < list.size(); i++) {
            const wcstring &dir = list.at(i);
            if (!string_prefixes_string(L"/", dir) || contains(existing_values, dir)) {
                any_success = true;
                continue;
            }

            int show_hint = 0;
            bool error = false;
            struct stat buff;
            if (wstat(dir, &buff) == -1) {
                error = true;
            } else if (!S_ISDIR(buff.st_mode)) {
                error = true;
                errno = ENOTDIR;
            } else if (waccess(dir, X_OK) == -1) {
                error = true;
            }

            if (!error) {
                any_success = true;
            } else {
                streams.err.append_format(_(BUILTIN_SET_PATH_ERROR), L"set", key, dir.c_str(),
                                          strerror(errno));
                const wchar_t *colon = wcschr(dir.c_str(), L':');

                if (colon && *(colon + 1)) {
                    show_hint = 1;
                }
            }

            if (show_hint) {
                streams.err.append_format(_(BUILTIN_SET_PATH_HINT), L"set", key, key,
                                          wcschr(dir.c_str(), L':') + 1);
            }
        }

        // Fail at setting the path if we tried to set it to something non-empty, but it wound up
        // empty.
        if (!list.empty() && !any_success) {
            return STATUS_CMD_ERROR;
        }
    }

    // We don't check `val->empty()` because an array var with a single empty string will be
    // "empty". A truly empty array var is set to the special value `ENV_NULL`.
    auto val = list_to_array_val(list);
    int retcode = env_set(key, *val == ENV_NULL ? NULL : val->c_str(), scope | ENV_USER);
    switch (retcode) {
        case ENV_OK: {
            retcode = STATUS_CMD_OK;
            break;
        }
        case ENV_PERM: {
            streams.err.append_format(_(L"%ls: Tried to change the read-only variable '%ls'\n"),
                                      L"set", key);
            retcode = STATUS_CMD_ERROR;
            break;
        }
        case ENV_SCOPE: {
            streams.err.append_format(
                _(L"%ls: Tried to set the special variable '%ls' with the wrong scope\n"), L"set",
                key);
            retcode = STATUS_CMD_ERROR;
            break;
        }
        case ENV_INVALID: {
            streams.err.append_format(
                _(L"%ls: Tried to set the special variable '%ls' to an invalid value\n"), L"set",
                key);
            retcode = STATUS_CMD_ERROR;
            break;
        }
        default: {
            DIE("unexpected env_set() ret val");
            break;
        }
    }

    return retcode;
}

/// Extract indexes from a destination argument of the form name[index1 index2...]
///
/// \param indexes the list to insert the new indexes into
/// \param src the source string to parse
/// \param name the name of the element. Return null if the name in \c src does not match this name
/// \param var_count the number of elements in the array to parse.
///
/// \return the total number of indexes parsed, or -1 on error
static int parse_index(std::vector<long> &indexes, const wchar_t *src, const wchar_t *name,
                       size_t var_count, io_streams_t &streams) {
    size_t len;
    int count = 0;
    const wchar_t *src_orig = src;

    if (src == 0) {
        return 0;
    }

    while (*src != L'\0' && (iswalnum(*src) || *src == L'_')) src++;

    if (*src != L'[') {
        streams.err.append_format(_(BUILTIN_SET_ARG_COUNT), L"set");
        return 0;
    }

    len = src - src_orig;
    if ((wcsncmp(src_orig, name, len) != 0) || (wcslen(name) != (len))) {
        streams.err.append_format(
            _(L"%ls: Multiple variable names specified in single call (%ls and %.*ls)\n"), L"set",
            name, len, src_orig);
        return 0;
    }

    src++;
    while (iswspace(*src)) src++;

    while (*src != L']') {
        const wchar_t *end;
        long l_ind = fish_wcstol(src, &end);
        if (errno > 0) {  // ignore errno == -1 meaning the int did not end with a '\0'
            streams.err.append_format(_(L"%ls: Invalid index starting at '%ls'\n"), L"set", src);
            return 0;
        }

        if (l_ind < 0) l_ind = var_count + l_ind + 1;

        src = end;  //!OCLINT(parameter reassignment)
        if (*src == L'.' && *(src + 1) == L'.') {
            src += 2;
            long l_ind2 = fish_wcstol(src, &end);
            if (errno > 0) {  // ignore errno == -1 meaning the int did not end with a '\0'
                return 1;
            }
            src = end;  //!OCLINT(parameter reassignment)

            if (l_ind2 < 0) {
                l_ind2 = var_count + l_ind2 + 1;
            }
            int direction = l_ind2 < l_ind ? -1 : 1;
            for (long jjj = l_ind; jjj * direction <= l_ind2 * direction; jjj += direction) {
                // debug(0, L"Expand range [set]: %i\n", jjj);
                indexes.push_back(jjj);
                count++;
            }
        } else {
            indexes.push_back(l_ind);
            count++;
        }

        while (iswspace(*src)) src++;
    }

    return count;
}

static int update_values(wcstring_list_t &list, std::vector<long> &indexes,
                         wcstring_list_t &values) {
    size_t i;

    // Replace values where needed.
    for (i = 0; i < indexes.size(); i++) {
        // The '- 1' below is because the indices in fish are one-based, but the vector uses
        // zero-based indices.
        long ind = indexes[i] - 1;
        const wcstring newv = values[i];
        if (ind < 0) {
            return 1;
        }
        if ((size_t)ind >= list.size()) {
            list.resize(ind + 1);
        }

        // free((void *) al_get(list, ind));
        list[ind] = newv;
    }

    return 0;
}

/// Erase from a list of wcstring values at specified indexes.
static void erase_values(wcstring_list_t &list, const std::vector<long> &indexes) {
    // Make a set of indexes.
    // This both sorts them into ascending order and removes duplicates.
    const std::set<long> indexes_set(indexes.begin(), indexes.end());

    // Now walk the set backwards, so we encounter larger indexes first, and remove elements at the
    // given (1-based) indexes.
    std::set<long>::const_reverse_iterator iter;
    for (iter = indexes_set.rbegin(); iter != indexes_set.rend(); ++iter) {
        long val = *iter;
        if (val > 0 && (size_t)val <= list.size()) {
            // One-based indexing!
            list.erase(list.begin() + val - 1);
        }
    }
}

/// Print the names of all environment variables in the scope, with or without shortening, with or
/// without values, with or without escaping
static void print_variables(int include_values, int esc, bool shorten_ok, int scope,
                            io_streams_t &streams) {
    wcstring_list_t names = env_get_names(scope);
    sort(names.begin(), names.end());

    for (size_t i = 0; i < names.size(); i++) {
        const wcstring key = names.at(i);
        const wcstring e_key = escape_string(key, 0);

        streams.out.append(e_key);

        if (include_values) {
            env_var_t value = env_get_string(key, scope);
            if (!value.missing()) {
                int shorten = 0;

                if (shorten_ok && value.length() > 64) {
                    shorten = 1;
                    value.resize(60);
                }

                wcstring e_value = esc ? expand_escape_variable(value) : value;

                streams.out.append(L" ");
                streams.out.append(e_value);

                if (shorten) {
                    streams.out.push_back(ellipsis_char);
                }
            }
        }

        streams.out.append(L"\n");
    }
}

static void show_scope(const wchar_t *var_name, int scope, io_streams_t &streams) {
    const wchar_t *scope_name;
    switch (scope) {
        case ENV_LOCAL: {
            scope_name = L"local";
            break;
        }
        case ENV_GLOBAL: {
            scope_name = L"global";
            break;
        }
        case ENV_UNIVERSAL: {
            scope_name = L"universal";
            break;
        }
        default: {
            DIE("invalid scope");
            break;
        }
    }

    if (env_exist(var_name, scope)) {
        const env_var_t evar = env_get_string(var_name, scope | ENV_EXPORT | ENV_USER);
        const wchar_t *exportv = evar.missing() ? _(L"unexported") : _(L"exported");

        const env_var_t var = env_get_string(var_name, scope | ENV_USER);
        wcstring_list_t result;
        if (!var.empty()) tokenize_variable_array(var, result);

        streams.out.append_format(_(L"$%ls: set in %ls scope, %ls, with %d elements\n"), var_name,
                                  scope_name, exportv, result.size());
        for (size_t i = 0; i < result.size(); i++) {
            if (result.size() > 100) {
                if (i == 50) streams.out.append(L"...\n");
                if (i >= 50 && i < result.size() - 50) continue;
            }
            const wcstring value = result[i];
            const wcstring escaped_val =
                escape_string(value.c_str(), ESCAPE_NO_QUOTED, STRING_STYLE_SCRIPT);
            streams.out.append_format(_(L"$%ls[%d]: length=%d value=|%ls|\n"), var_name, i + 1,
                                      value.size(), escaped_val.c_str());
        }
    } else {
        streams.out.append_format(_(L"$%ls: not set in %ls scope\n"), var_name, scope_name);
    }
}

/// Show mode. Show information about the named variable(s).
static int builtin_set_show(const wchar_t *cmd, int argc, wchar_t **argv,
                            parser_t &parser, io_streams_t &streams) {
    if (argc == 0) {  // show all vars
        wcstring_list_t names = env_get_names(ENV_USER);
        sort(names.begin(), names.end());
        for (auto it : names) {
            show_scope(it.c_str(), ENV_LOCAL, streams);
            show_scope(it.c_str(), ENV_GLOBAL, streams);
            show_scope(it.c_str(), ENV_UNIVERSAL, streams);
            streams.out.push_back(L'\n');
        }
    } else {
        for (int i = 0; i < argc; i++) {
            wchar_t *arg = argv[i];

            if (!valid_var_name(arg)) {
                streams.err.append_format(_(L"$%ls: invalid var name\n"), arg);
                continue;
            }

            if (wcschr(arg, L'[')) {
                streams.err.append_format(
                    _(L"%ls: `set --show` does not allow slices with the var names\n"), cmd);
                builtin_print_help(parser, streams, cmd, streams.err);
                return STATUS_CMD_ERROR;
            }

            show_scope(arg, ENV_LOCAL, streams);
            show_scope(arg, ENV_GLOBAL, streams);
            show_scope(arg, ENV_UNIVERSAL, streams);
            streams.out.push_back(L'\n');
        }
    }

    return STATUS_CMD_OK;
}

/// The set builtin creates, updates, and erases (removes, deletes) variables.
int builtin_set(parser_t &parser, io_streams_t &streams, wchar_t **argv) {
    wchar_t *cmd = argv[0];
    int argc = builtin_count_args(argv);

    // Flags to set the work mode.
    int local = 0, global = 0, exportv = 0;
    int erase = 0, list = 0, unexport = 0;
    int universal = 0, query = 0;
    bool show = false;
    bool shorten_ok = true;
    bool preserve_failure_exit_status = true;
    const int incoming_exit_status = proc_get_last_status();

    // Variables used for performing the actual work.
    wchar_t *dest = NULL;
    int retcode = STATUS_CMD_OK;
    int scope;
    int slice = 0;

    // Variables used for parsing the argument list. This command is atypical in using the "+"
    // (REQUIRE_ORDER) option for flag parsing. This is not typical of most fish commands. It means
    // we stop scanning for flags when the first non-flag argument is seen.
    static const wchar_t *short_options = L"+:LSUeghlnqux";
    static const struct woption long_options[] = {{L"export", no_argument, NULL, 'x'},
                                                  {L"global", no_argument, NULL, 'g'},
                                                  {L"local", no_argument, NULL, 'l'},
                                                  {L"erase", no_argument, NULL, 'e'},
                                                  {L"names", no_argument, NULL, 'n'},
                                                  {L"unexport", no_argument, NULL, 'u'},
                                                  {L"universal", no_argument, NULL, 'U'},
                                                  {L"long", no_argument, NULL, 'L'},
                                                  {L"query", no_argument, NULL, 'q'},
                                                  {L"show", no_argument, NULL, 'S'},
                                                  {L"help", no_argument, NULL, 'h'},
                                                  {NULL, 0, NULL, 0}};

    // Parse options to obtain the requested operation and the modifiers.
    int opt;
    wgetopter_t w;
    while ((opt = w.wgetopt_long(argc, argv, short_options, long_options, NULL)) != -1) {
        switch (opt) {
            case 'e': {
                erase = 1;
                preserve_failure_exit_status = false;
                break;
            }
            case 'n': {
                list = 1;
                preserve_failure_exit_status = false;
                break;
            }
            case 'x': {
                exportv = 1;
                break;
            }
            case 'l': {
                local = 1;
                break;
            }
            case 'g': {
                global = 1;
                break;
            }
            case 'u': {
                unexport = 1;
                break;
            }
            case 'U': {
                universal = 1;
                break;
            }
            case 'L': {
                shorten_ok = false;
                break;
            }
            case 'q': {
                query = 1;
                preserve_failure_exit_status = false;
                break;
            }
            case 'S': {
                show = true;
                preserve_failure_exit_status = false;
                break;
            }
            case 'h': {
                builtin_print_help(parser, streams, cmd, streams.out);
                return STATUS_CMD_OK;
            }
            case ':': {
                builtin_missing_argument(parser, streams, cmd, argv[w.woptind - 1]);
                return STATUS_INVALID_ARGS;
            }
            case '?': {
                builtin_unknown_option(parser, streams, cmd, argv[w.woptind - 1]);
                return STATUS_INVALID_ARGS;
            }
            default: {
                DIE("unexpected retval from wgetopt_long");
                break;
            }
        }
    }

    if (show) {
        argc -= w.woptind;
        argv += w.woptind;
        return builtin_set_show(cmd, argc, argv, parser, streams);
    }

    // Ok, all arguments have been parsed, let's validate them. If we are checking the existance of
    // a variable (-q) we can not also specify scope.
    if (query && (erase || list)) {
        streams.err.append_format(BUILTIN_ERR_COMBO, cmd);
        builtin_print_help(parser, streams, cmd, streams.err);
        return STATUS_INVALID_ARGS;
    }

    // We can't both list and erase variables.
    if (erase && list) {
        streams.err.append_format(BUILTIN_ERR_COMBO, cmd);
        builtin_print_help(parser, streams, cmd, streams.err);
        return STATUS_INVALID_ARGS;
    }

    // Variables can only have one scope.
    if (local + global + universal > 1) {
        streams.err.append_format(BUILTIN_ERR_GLOCAL, cmd);
        builtin_print_help(parser, streams, cmd, streams.err);
        return STATUS_INVALID_ARGS;
    }

    // Variables can only have one export status.
    if (exportv && unexport) {
        streams.err.append_format(BUILTIN_ERR_EXPUNEXP, argv[0]);
        builtin_print_help(parser, streams, cmd, streams.err);
        return STATUS_INVALID_ARGS;
    }

    // Calculate the scope value for variable assignement.
    scope = (local ? ENV_LOCAL : 0) | (global ? ENV_GLOBAL : 0) | (exportv ? ENV_EXPORT : 0) |
            (unexport ? ENV_UNEXPORT : 0) | (universal ? ENV_UNIVERSAL : 0) | ENV_USER;

    if (query) {
        // Query mode. Return the number of variables that do not exist out of the specified
        // variables.
        int i;
        for (i = w.woptind; i < argc; i++) {
            wchar_t *arg = argv[i];
            int slice = 0;

            dest = wcsdup(arg);
            assert(dest);

            if (wcschr(dest, L'[')) {
                slice = 1;
                *wcschr(dest, L'[') = 0;
            }

            if (slice) {
                std::vector<long> indexes;
                wcstring_list_t result;
                size_t j;

                env_var_t dest_str = env_get_string(dest, scope);
                if (!dest_str.missing()) tokenize_variable_array(dest_str, result);

                if (!parse_index(indexes, arg, dest, result.size(), streams)) {
                    builtin_print_help(parser, streams, cmd, streams.err);
                    retcode = 1;
                    break;
                }
                for (j = 0; j < indexes.size(); j++) {
                    long idx = indexes[j];
                    if (idx < 1 || (size_t)idx > result.size()) {
                        retcode++;
                    }
                }
            } else {
                if (!env_exist(arg, scope)) {
                    retcode++;
                }
            }

            free(dest);
        }
        return retcode;
    }

    if (list) {
        // Maybe we should issue an error if there are any other arguments?
        print_variables(0, 0, shorten_ok, scope, streams);
        return STATUS_CMD_OK;
    }

    if (w.woptind == argc) {
        // Print values of variables.
        if (erase) {
            streams.err.append_format(_(L"%ls: Erase needs a variable name\n"), cmd);
            builtin_print_help(parser, streams, cmd, streams.err);
            retcode = STATUS_INVALID_ARGS;
        } else {
            print_variables(1, 1, shorten_ok, scope, streams);
        }

        return retcode;
    }

    dest = wcsdup(argv[w.woptind]);
    assert(dest);

    if (wcschr(dest, L'[')) {
        slice = 1;
        *wcschr(dest, L'[') = 0;
    }

    if (!valid_var_name(dest)) {
        streams.err.append_format(BUILTIN_ERR_VARNAME, cmd, dest);
        builtin_print_help(parser, streams, argv[0], streams.err);
        return STATUS_INVALID_ARGS;
    }

    // Set assignment can work in two modes, either using slices or using the whole array. We detect
    // which mode is used here.
    if (slice) {
        // Slice mode.
        std::vector<long> indexes;
        wcstring_list_t result;

        const env_var_t dest_str = env_get_string(dest, scope);
        if (!dest_str.missing()) {
            tokenize_variable_array(dest_str, result);
        } else if (erase) {
            retcode = 1;
        }

        if (!retcode) {
            for (; w.woptind < argc; w.woptind++) {
                if (!parse_index(indexes, argv[w.woptind], dest, result.size(), streams)) {
                    builtin_print_help(parser, streams, argv[0], streams.err);
                    retcode = 1;
                    break;
                }

                size_t idx_count = indexes.size();
                size_t val_count = argc - w.woptind - 1;

                if (!erase) {
                    if (val_count < idx_count) {
                        streams.err.append_format(_(BUILTIN_SET_ARG_COUNT), argv[0]);
                        builtin_print_help(parser, streams, argv[0], streams.err);
                        retcode = 1;
                        break;
                    }
                    if (val_count == idx_count) {
                        w.woptind++;
                        break;
                    }
                }
            }
        }

        if (!retcode) {
            // Slice indexes have been calculated, do the actual work.
            if (erase) {
                erase_values(result, indexes);
                my_env_set(dest, result, scope, streams);
            } else {
                wcstring_list_t value;

                while (w.woptind < argc) {
                    value.push_back(argv[w.woptind++]);
                }

                if (update_values(result, indexes, value)) {
                    streams.err.append_format(L"%ls: ", argv[0]);
                    streams.err.append_format(ARRAY_BOUNDS_ERR);
                    streams.err.push_back(L'\n');
                }

                my_env_set(dest, result, scope, streams);
            }
        }
    } else {
        w.woptind++;
        // No slicing.
        if (erase) {
            if (w.woptind != argc) {
                streams.err.append_format(_(L"%ls: Values cannot be specfied with erase\n"),
                                          argv[0]);
                builtin_print_help(parser, streams, argv[0], streams.err);
                retcode = 1;
            } else {
                retcode = env_remove(dest, scope);
            }
        } else {
            wcstring_list_t val;
            for (int i = w.woptind; i < argc; i++) val.push_back(argv[i]);
            retcode = my_env_set(dest, val, scope, streams);
        }
    }

    // Check if we are setting variables above the effective scope. See
    // https://github.com/fish-shell/fish-shell/issues/806
    env_var_t global_dest = env_get_string(dest, ENV_GLOBAL);
    if (universal && !global_dest.missing() && shell_is_interactive()) {
        streams.err.append_format(
            _(L"%ls: Universal var '%ls' created but shadowed by global var of the same name.\n"),
            L"set", dest);
    }

    free(dest);

    if (retcode == STATUS_CMD_OK && preserve_failure_exit_status) retcode = incoming_exit_status;
    return retcode;
}
