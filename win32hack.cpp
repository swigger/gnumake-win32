﻿#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>
extern "C" {
#define export export1  // export is a keyword in C++
#include "makeint.h"
#include "variable.h"
}
#include <Windows.h>

using std::vector;
using std::string;

struct arg_t {
	bool hasval;
	std::string val;

	void reset() {
		hasval = 0;
		val.clear();
	}
};

static void commit(std::vector<std::string>& args, arg_t & cur)
{
	if (cur.hasval || !cur.val.empty())
		args.push_back(cur.val);
	cur.reset();
}

static bool find_exe(const std::string& arg0, struct file* file) {
	// may be powershell command.
	auto env = target_environment(file, 0);
	char* path = nullptr;
	for (int i = 0; env && env[i]; ++i)
	{
		if (strnicmp(env[i], "path=", 5) == 0)
		{
			path = env[i] + 5;
			break;
		}
	}
	char* ps = path;
	char* p = ps;
	char* pe = path + strlen(path);
	std::string tmp_fn;
	bool found_file = false;
	for (; p < pe; ++p)
	{
		if (*p == ';')
		{
			if (p > ps)
			{
				tmp_fn.assign(ps, p);
				if (tmp_fn.back() != '/' && tmp_fn.back() != '\\')
					tmp_fn += '\\';
				tmp_fn += arg0;
				DWORD attr = GetFileAttributesA(tmp_fn.c_str());
				if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0)
				{
					found_file = true;
					break;
				}
			}
			ps = p + 1;
		}
	}
	return found_file;
}

extern "C" char**
construct_command_argv(char* line, char** restp, struct file* file, int cmd_flags, char** batch_filename)
{
	std::vector<std::string> args;
	arg_t cur;
	string allcmd;
	bool in_quote = false;
	for (int i = 0; line[i]; ++i)
	{
		char ch = line[i];
		if (!in_quote && ch == '\n' && restp)
		{
			if (line[i+1])
				*restp = line + i;
			break;
		}
		if (!in_quote && ch > 0 && isspace(ch))
		{
			commit(args, cur);
			allcmd += ch;
			continue;
		}

		if (ch == '"')
		{
			cur.hasval = 1;
			in_quote = !in_quote;
			allcmd += ch;
		}
		else if (ch == '\\')
		{
			// 采用CMD风格的转义处理方式 \只用来转义"或\，但转义自己的前提是后面跟着"
			//  \\\\" 是转义 \\\\不是转义  \"是转义
			//  如果遇到了\，就一直看是不是"跟着，否则原样处理
			bool is_tr = false;
			int j = i + 1;
			// add special case for '\\', '\n', ANYTHING, gnumake's multi-line mode.
			if (line[j] == '\n' && !in_quote && line[j + 1] != 0) {
				++i;
				continue;
			}
			for (; line[j]; ++j)
			{
				if (line[j] == '\\');
				else if (line[j] == '"')
				{
					is_tr = 1;
				}
				else break;
			}
			if (is_tr)
			{
				bool in_tr = true;
				for (j = i + 1; line[j]=='\\'; ++j)
				{
					if (in_tr)
					{
						cur.val += line[j];
						in_tr = 0;
					}
					else
						in_tr = 1;
				}
				if (in_tr)
					cur.val += line[j++];
			}
			else
			{
				for (j = i; line[j] == '\\'; ++j)
					cur.val += line[j];
			}
			allcmd.append(line + i, j - i);
			i = j - 1; //note: ++i in for loop!
		}
		else
		{
			// in_quote or normal chars.
			cur.val += ch;
			allcmd += ch;
		}
	}
	commit(args, cur);
	if (args.empty())
	{
		char** ar = (char**)malloc(sizeof(char*)*4);
		ar[0] = strdup("cmd"); //ar[0] is passed to free
		ar[1] = (char*)"/c";
		ar[2] = (char*)"rem";
		ar[3] = 0;
		return ar;
	}

	bool is_complex = false;
	for (auto& s : args) {
		if (s == "|" || s == "&" || s == "&&" || s == "||" || s == "<" || s == ">" || s == ">>" ||
			s == ";" || s == "{" || s == "}")
		{
			is_complex = true;
			break;
		}
	}

	static const char* sh_cmds_dos[] =
	{ "assoc", "break", "call", "cd", "chcp", "chdir", "cls", "color", "copy",
	  "ctty", "date", "del", "dir", "echo", "echo.", "endlocal", "erase",
	  "exit", "for", "ftype", "goto", "if", "md", "mkdir", "mklink", "move",
	  "path", "pause", "prompt", "rd", "rem", "ren", "rename", "rmdir",
	  "set", "setlocal", "shift", "time", "title", "type", "ver", "verify",
	  "vol", ":", 0 };
	constexpr uint32_t n_shcmds = sizeof(sh_cmds_dos) / sizeof(const char*) - 1;
	auto ptr = std::lower_bound(&sh_cmds_dos[0], &sh_cmds_dos[n_shcmds], args[0].c_str(), [](const char* a, const char* b) {
		return strcmp(a, b) < 0;
		});
	if (*ptr && args[0] == *ptr)
	{
		args.clear();
		args.push_back("cmd");
		args.push_back("/c");
		args.push_back(line);
	}
	else if (!is_complex && find_exe(args[0], file))
	{
		; // use args as is.
	}
	else
	{
		// assume powershell.
		args.clear();
		if (restp) *restp = 0;
		args.push_back("pwsh");
		args.push_back("-Command");
		auto p1 = allcmd.c_str();
		while (*p1 && isspace(*p1)) ++p1;
		if (*p1 == ';') ++p1;
		while (*p1 && isspace(*p1)) ++p1;
		args.push_back(p1);
	}

	// collect args.
	char** ar;
	{
		size_t sz = 0;
		for (auto& s : args)
		{
			sz += s.size() + 1;
		}
		ar = (char**)calloc(args.size() + 1, sizeof(char*));
		char* store = (char*)malloc(sz + 1);
		for (size_t i = 0; i < args.size(); ++i)
		{
			memcpy(store, args[i].c_str(), args[i].length() + 1);
			ar[i] = store;
			store += args[i].length() + 1;
		}
	}
	return ar;
}

#define extc extern "C"
char* comma_list(const char* nm, unsigned int argc, char** argv)
{
	// return malloc-ed memory.
	string ret;
	vector<string> toks;
	const char* ins = argv[0];
	if (ins && *ins) {
		const char* p = ins;
		const char* pe = ins + strlen(ins);
		enum state_t { NORMAL, IN_QUOTE } state = NORMAL;
		for (; p < pe; ++p) {
			if (state == NORMAL) {
				if (isspace((uint8_t) *p)) {
					if (p > ins) toks.push_back(string(ins, p));
					ins = p + 1;
				}
				else if (*p == '"') {
					state = IN_QUOTE;
				}
			}
			else if (state == IN_QUOTE) {
				if (*p == '\\') {
					if (p[1] == 0) break;
					else ++p;
				}
				else if (*p == '"') {
					state = NORMAL;
					toks.push_back(string(ins, p));
					ins = p + 1;
				}
			}
		}
		if (ins < pe) toks.push_back(string(ins, pe));
	}
	if (!toks.empty()) {
		ret = toks[0];
		for (size_t i = 1; i < toks.size(); ++i) {
			ret += ", ";
			ret += toks[i];
		}
	}
	char* rr = (char*) xmalloc(ret.length() + 1);
	memcpy(rr, ret.c_str(), ret.length() + 1);
	return rr;
}

extc void add_ex_funcs(void) {
	define_new_function(0, "comma_list", 1, 1, 0, comma_list);
}
