/*
* Copyright (c) 2016, Egor Pugin
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*     1. Redistributions of source code must retain the above copyright
*        notice, this list of conditions and the following disclaimer.
*     2. Redistributions in binary form must reproduce the above copyright
*        notice, this list of conditions and the following disclaimer in the
*        documentation and/or other materials provided with the distribution.
*     3. Neither the name of the copyright holder nor the names of
*        its contributors may be used to endorse or promote products
*        derived from this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "autotools.h"

#include "../checks_detail.h"
#include "../common.h"

#include <boost/algorithm/string.hpp>

#include <iostream>
#include <regex>
#include <vector>

/*
TODO

AC_LINK_IFELSE(   [AC_LANG_PROGRAM([[]], [[
unsigned int fpcr;
__asm__ __volatile__ ("fmove.l %%fpcr,%0" : "=g" (fpcr));
__asm__ __volatile__ ("fmove.l %0,%%fpcr" : : "g" (fpcr));
]])],[have_gcc_asm_for_mc68881=yes],[have_gcc_asm_for_mc68881=no])
AC_MSG_RESULT($have_gcc_asm_for_mc68881)
if test "$have_gcc_asm_for_mc68881" = yes
then
AC_DEFINE(HAVE_GCC_ASM_FOR_MC68881, 1,
[Define if we can use gcc inline assembler to get and set mc68881 fpcr])
fi
*/

struct command
{
    String name;
    std::vector<String> params;
};

struct if_expr
{
    struct if_action
    {
        String var;
        bool equ;
        String value;
        String action;
    };

    if_action if_actions;
    std::vector<if_action> if_else_actions;
    String else_actions;
};

struct ac_processor
{
    using value = std::pair<String, bool>;

    String file;
    std::vector<command> commands;
    Checks checks;
    std::map<String, std::set<value>> vars;
    std::map<String, if_expr> conditions;

    ac_processor(const path &p);

    template <class T>
    void split_and_add(command &c, std::function<bool(String)> fun = std::function<bool(String)>());
    template <class T>
    void ifdef_add(command &c);

    void process();
    void process_AC_CHECK_FUNCS(command &c);
    void process_AC_CHECK_DECLS(command &c);
    void process_AC_COMPILE_IFELSE(command &c);
    void process_AC_RUN_IFELSE(command &c);
    void process_AC_TRY_RUN(command &c);
    void process_AC_CHECK_HEADERS(command &c);
    void process_AC_CHECK_TYPES(command &c);
    void process_AC_HEADER_DIRENT(command &c);
    void process_AC_HEADER_TIME(command &c);
    void process_AC_HEADER_STDC(command &c);
    void process_AC_HEADER_MAJOR(command &c);
    void process_AC_STRUCT_TM(command &c);
    void process_AC_STRUCT_TIMEZONE(command &c);
    void process_AC_CHECK_LIB(command &c);
    void process_AC_CHECK_MEMBERS(command &c);
};

int get_end_of_block(const String &s, int i = 1)
{
    auto c = s[i - 1];
    int n_curly = c == '(';
    int n_square = c == '[';
    int n_quotes = c == '\"';
    auto sz = (int)s.size();
    while ((n_curly > 0 || n_square > 0 || n_quotes > 0) && i < sz)
    {
        c = s[i];

        if (c == '\"')
        {
            if (n_quotes == 0)
                i = get_end_of_block(s.c_str(), i + 1) - 1;
            else if (s[i - 1] == '\\')
                ;
            else
                n_quotes--;
        }
        else
        {
            switch (c)
            {
            case '(':
            case '[':
                i = get_end_of_block(s.c_str(), i + 1) - 1;
                break;
            case ')':
                n_curly--;
                break;
            case ']':
                n_square--;
                break;
            }
        }

        i++;
    }
    return i;
}

auto parse_arguments(const String &f)
{
    int start = 0;
    int i = 0;
    int sz = (int)f.size();
    std::vector<String> s;

    auto add_to_s = [&s](auto &str)
    {
        boost::trim(str);
        if (str.empty())
            return;
        while (!str.empty() && str.front() == '[' && str.back() == ']')
            str = str.substr(1, str.size() - 2);
        s.push_back(str);
    };

    while (i < sz)
    {
        auto c = f[i];
        switch (c)
        {
        case '\"':
            i = get_end_of_block(f, i + 1) - 1;
            break;
        case ',':
            add_to_s(f.substr(start, i - start));
            start = i + 1;
            break;
        case '(':
        case '[':
            i = get_end_of_block(f, i + 1) - 1;
            add_to_s(f.substr(start, i - start + 1));
            start = i + 1;
            break;
        }
        i++;
    }
    add_to_s(f.substr(start, i - start));
    return s;
}

auto parse_command(const String &f)
{
    auto i = get_end_of_block(f.c_str());
    auto s = f.substr(1, i - 2);
    boost::trim(s);
    return parse_arguments(s);
}

auto parse_configure_ac(String f)
{
    auto ac = {
        "AC_CHECK_\\w+",
        //"AC_EGREP_\\w+",
        "AC_TRY_\\w+",
        "AC_\\w+?_IFELSE",
        "AC_HEADER_\\w+",
        "AC_STRUCT_\\w+",
    };

    String rx = "(";
    for (auto &a : ac)
    {
        rx += a;
        rx += "|";
    }
    rx.resize(rx.size() - 1);
    rx += ")";

    std::regex r(rx);
    std::smatch m;
    std::vector<command> commands;
    while (std::regex_search(f, m, r))
    {
        command cmd;
        cmd.name = m[1].str();
        f = m.suffix();
        if (f[0] == '(')
            cmd.params = parse_command(f);
        commands.push_back(cmd);
    }
    return commands;
}

auto parse_conditions(String f)
{
    std::map<String, if_expr> conds;

    String s = f;
    std::regex r_if(R"rrr(if\s+test\s+"?\$(\w+)"?\s+(\S+)\s+(\w+))rrr");
    std::smatch m;
    while (std::regex_search(s, m, r_if))
    {
        auto var = m[1].str();
        auto sign = m[2].str();
        auto val = m[3].str();
        conds[var].if_actions.var = var;
        conds[var].if_actions.equ = sign == "=";
        if (sign != "=" && sign != "!=")
            std::cerr << "Unknown sign " << sign << "\n";
        conds[var].if_actions.value = val;
        s = m.suffix();

        auto i = s.find("if test");
        auto f = s.find("fi");
        int n = 0;
        while (i < f)
        {
            i = s.find("if test", i + 1);
            n++;
        }
        while (n--)
            f = s.find("fi", f + 1);
        conds[var].if_actions.action = s.substr(0, f);
        s = s.substr(f);
    }

    return conds;
}

void process_configure_ac(const path &p)
{
    ac_processor proc(p);
    proc.process();
    std::cout << proc.checks.save();
}

ac_processor::ac_processor(const path &p)
{
    file = read_file(p);

    std::regex dnl("dnl.*?\\n");
    file = std::regex_replace(file, dnl, "\n");

    commands = parse_configure_ac(file);
    conditions = parse_conditions(file);
}

void ac_processor::process()
{
    std::set<String> unproc;
    for (auto &c : commands)
    {

#define CASE(n1, n2)     \
    if (c.name == #n1)   \
    {                    \
        process_##n2(c); \
        continue;        \
    }

#define CASE_NOT_EMPTY(n1, n2) \
    if (c.name == #n1)         \
    {                          \
        if (c.params.empty())  \
            continue;          \
        process_##n2(c);       \
        continue;              \
    }

#define TWICE(m,x) m(x,x)

#define SILENCE(n1)    \
    if (c.name == #n1) \
    {                  \
        continue;      \
    }

        CASE_NOT_EMPTY(AC_CHECK_FUNC, AC_CHECK_FUNCS);
        TWICE(CASE_NOT_EMPTY, AC_CHECK_FUNCS);

        CASE_NOT_EMPTY(AC_CHECK_DECL, AC_CHECK_DECLS);
        TWICE(CASE_NOT_EMPTY, AC_CHECK_DECLS);

        CASE_NOT_EMPTY(AC_LINK_IFELSE, AC_COMPILE_IFELSE);
        CASE_NOT_EMPTY(AC_PREPROC_IFELSE, AC_COMPILE_IFELSE);
        TWICE(CASE_NOT_EMPTY, AC_COMPILE_IFELSE);

        TWICE(CASE_NOT_EMPTY, AC_RUN_IFELSE);

        TWICE(CASE_NOT_EMPTY, AC_TRY_RUN);

        CASE_NOT_EMPTY(AC_CHECK_HEADER, AC_CHECK_HEADERS);
        TWICE(CASE_NOT_EMPTY, AC_CHECK_HEADERS);

        CASE_NOT_EMPTY(AC_CHECK_SIZEOF, AC_CHECK_TYPES);
        CASE_NOT_EMPTY(AC_CHECK_TYPE, AC_CHECK_TYPES);
        TWICE(CASE_NOT_EMPTY, AC_CHECK_TYPES);

        TWICE(CASE, AC_HEADER_DIRENT);
        TWICE(CASE, AC_HEADER_TIME);
        TWICE(CASE, AC_HEADER_STDC);
        TWICE(CASE, AC_HEADER_MAJOR);

        TWICE(CASE, AC_STRUCT_TM);
        TWICE(CASE, AC_STRUCT_TIMEZONE);

        TWICE(CASE, AC_CHECK_LIB);

        CASE_NOT_EMPTY(AC_CHECK_MEMBER, AC_CHECK_MEMBERS);
        TWICE(CASE, AC_CHECK_MEMBERS);

        SILENCE(AC_CHECK_PROG);
        SILENCE(AC_CHECK_PROGS);
        SILENCE(AC_CHECK_TOOLS);
        SILENCE(AC_CHECK_FILE);

        if (unproc.find(c.name) == unproc.end())
        {
            std::cerr << "Unprocessed statement: " << c.name << "\n";
            unproc.insert(c.name);
        }
    }
}

template <class T>
void ac_processor::split_and_add(command &c, std::function<bool(String)> fun)
{
    boost::replace_all(c.params[0], "\\", "\n");
    boost::replace_all(c.params[0], "\t", "\n");
    boost::replace_all(c.params[0], " ", "\n");
    boost::replace_all(c.params[0], ",", "\n");
    auto funcs = split_lines(c.params[0]);
    for (auto &f : funcs)
    {
        if (!fun || fun(f))
            checks.addCheck<T>(f);
    }
}

template <class T>
void ac_processor::ifdef_add(command &c)
{
    static std::regex r_kv("[\\d\\w-]+=[\\d\\w-]+");

    String var;
    String input = c.params[0];
    if (c.params[0].find("AC_") == 0)
    {
        auto cmd = c.params[0].substr(0, c.params[0].find('('));
        if (cmd == "AC_LANG_PROGRAM")
        {
            auto params = parse_arguments(c.params[0].substr(cmd.size() + 1));
            input = params[0] + "\n\n int main() { \n\n";
            if (params.size() > 1)
                input += params[1];
            input += "\n\n ; return 0; }";
        }
        else if (cmd == "AC_MSG_RESULT")
            ; // this is a printer
        else if (cmd == "AC_LANG_SOURCE")
        {
            auto params = parse_arguments(c.params[0].substr(cmd.size() + 1));
            input = params[0];
        }
        else if (cmd == "AC_LANG_CALL")
        {
            auto params = parse_arguments(c.params[0].substr(cmd.size() + 1));
            input = params[0] + "\n\n int main() { \n\n";
            if (params.size() > 1)
                input += params[1] + "()";
            input += "\n\n ; return 0; }";
        }
        else
        {
            std::cerr << "Unhandled AC_ statement: " << cmd << "\n";
            return;
        }
    }
    if (c.params.size() > 1)
    {
        if (c.params[1].find("AC_") == 0)
        {
            auto cmd = c.params[1].substr(0, c.params[1].find('('));
            if (cmd == "AC_MSG_RESULT")
                ; // this is a printer
            else if (cmd == "AC_DEFINE")
            {
                auto params = parse_arguments(c.params[1].substr(cmd.size() + 1));
                var = params[0];
            }
            else
                std::cerr << "Unhandled AC_ statement: " << cmd << "\n";
            return;
        }
        else
        {
            // if key-value?
            if (std::regex_match(c.params[1], r_kv))
            {
                auto p = c.params[1].find('=');
                auto key = c.params[1].substr(0, p);
                auto value = c.params[1].substr(p + 1);
                vars[key].insert({ value, true });
            }
            else
                return;
        }
    }
    if (c.params.size() > 2)
    {
        if (c.params[2].find("AC_") == 0)
        {
            auto cmd = c.params[2].substr(0, c.params[2].find('('));
            if (cmd == "AC_MSG_RESULT")
                ; // this is a printer
            else if (cmd == "AC_DEFINE")
            {
                auto params = parse_arguments(c.params[2].substr(cmd.size() + 1));
                //var = params[0]; // this could be very dangerous
            }
            else
                std::cerr << "Unhandled AC_ statement: " << cmd << "\n";
            return;
        }
        else
        {
            // if key-value?
            if (std::regex_match(c.params[2], r_kv))
            {
                auto p = c.params[2].find('=');
                auto key = c.params[2].substr(0, p);
                auto value = c.params[2].substr(p + 1);
                vars[key].insert({ value, false });
            }
            else
                return;
        }
    }
    if (var.empty() || input.empty())
        return;
    checks.addCheck<T>(var, input);
}

void ac_processor::process_AC_CHECK_FUNCS(command &c)
{
    split_and_add<CheckFunction>(c);
}

void ac_processor::process_AC_CHECK_DECLS(command &c)
{
    // TODO: add CheckDecl, HAVE_DECL_*
    // when not found HAVE_DECL_symbol is defined to ‘0’ instead of leaving HAVE_DECL_symbol undeclared
    process_AC_CHECK_FUNCS(c);
}

void ac_processor::process_AC_COMPILE_IFELSE(command &c)
{
    ifdef_add<CheckCSourceCompiles>(c);
}

void ac_processor::process_AC_RUN_IFELSE(command &c)
{
    ifdef_add<CheckCSourceRuns>(c);
}

void ac_processor::process_AC_TRY_RUN(command &c)
{
    ifdef_add<CheckCSourceRuns>(c);
}

void ac_processor::process_AC_CHECK_HEADERS(command &c)
{
    split_and_add<CheckInclude>(c);
}

void ac_processor::process_AC_CHECK_TYPES(command &c)
{
    split_and_add<CheckType>(c, [](auto &v)
    {
        if (v == "*" || v == "void")
            return false;
        return true;
    });
}

void ac_processor::process_AC_HEADER_DIRENT(command &c)
{
    process_AC_CHECK_HEADERS(command{ "", {"dirent.h","sys/ndir.h","sys/dir.h","ndir.h"} });
}

void ac_processor::process_AC_HEADER_TIME(command &c)
{
    process_AC_CHECK_HEADERS(command{ "",{ "time.h","sys/time.h" } });
    checks.addCheck<CheckCSourceCompiles>("HAVE_TIME_WITH_SYS_TIME", R"(
#include <time.h>
#include <sys/time.h>
int main() {return 0;}
)");
}

void ac_processor::process_AC_HEADER_STDC(command &c)
{
    process_AC_CHECK_HEADERS(command{ "",{ "stdlib.h", "stdarg.h", "string.h", "float.h" } });
    checks.addCheck<CheckCSourceCompiles>("STDC_HEADERS", R"(
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <float.h>
int main() {return 0;}
)");
}

void ac_processor::process_AC_HEADER_MAJOR(command &c)
{
    checks.addCheck<CheckCSourceCompiles>("MAJOR_IN_MKDEV", R"(
#include <sys/mkdev.h>
int main() { makedev(0, 0); return 0; }
)");

    checks.addCheck<CheckCSourceCompiles>("MAJOR_IN_SYSMACROS", R"(
#include <sys/sysmacros.h>
int main() { makedev(0, 0); return 0; }
)");
}

void ac_processor::process_AC_STRUCT_TM(command &c)
{
    auto p = checks.addCheck<CheckCSourceCompiles>("TM_IN_SYS_TIME", R"(
#include <time.h>
int main() { struct tm t; return 0; }
)");
    p->invert = true;
}

void ac_processor::process_AC_STRUCT_TIMEZONE(command &c)
{
    // Figure out how to get the current timezone.If struct tm has a tm_zone member,
    // define HAVE_STRUCT_TM_TM_ZONE(and the obsoleted HAVE_TM_ZONE).
    // Otherwise, if the external array tzname is found, define HAVE_TZNAME.
    checks.addCheck<CheckSymbol>("tzname", std::set<String>{ "time.h" });
}

void ac_processor::process_AC_CHECK_LIB(command &c)
{
    auto variable = "HAVE_" + boost::algorithm::to_upper_copy(c.params[1]);
    checks.addCheck<CheckCustom>(variable,
        "CHECK_LIBRARY_EXISTS(" + c.params[0] + " " + c.params[1] + " \"\" " + variable + ")");
}

void ac_processor::process_AC_CHECK_MEMBERS(command &c)
{
    auto variable = c.params[0];
    boost::replace_all(variable, "  ", " ");
    boost::replace_all(variable, " ", "_");
    boost::replace_all(variable, ".", "_");
    variable = "HAVE_" + boost::algorithm::to_upper_copy(variable);

    auto p = c.params[0].find('.');
    auto struct_ = c.params[0].substr(0, p);
    auto member = c.params[0].substr(p + 1);
    String header;
    if (struct_ == "struct stat")
        header = "sys/stat.h";
    // add more headers here

    checks.addCheck<CheckCustom>(variable,
        "CHECK_STRUCT_HAS_MEMBER(\"" + struct_ + "\" " + member + " \"" + header + "\" " + variable + ")");
}
