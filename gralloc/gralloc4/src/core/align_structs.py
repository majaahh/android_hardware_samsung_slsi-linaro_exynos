#!/usr/bin/env python3

import argparse
import sys

def parse_args():
	parser = argparse.ArgumentParser(description="Align structs on single lines in BEGIN...END ALIGNED SECTION blocks")
	parser.add_argument('file', metavar="FILE", nargs='?', help="If provided, a file to process.  Otherwise stdin")
	return parser.parse_args()

def braces_match(line):
	open_braces = 0
	for c in line:
		if c == '{':
			open_braces = open_braces + 1
		elif c == '}':
			if open_braces == 0:
				sys.exit("Too many close braces on line:\n{}", line)
			open_braces = open_braces - 1
	return (open_braces == 0)

assert(braces_match("{{{}}}"))
assert(not braces_match("{{}{}"))

def is_partial_comment(line):
	stripped = line.strip()
	return stripped.startswith("/*") and not stripped.endswith("*/")

assert(not is_partial_comment("/* */"))
assert(not is_partial_comment("/* /* */"))
assert(is_partial_comment("/* */ /"))
assert(is_partial_comment("/* */ /*"))

def is_line_ignored(line):
	stripped = line.lstrip()
	return (stripped == "") or stripped.startswith("/*") or stripped.startswith("//") or stripped.startswith("#")

def is_preprocessor(line):
	return line.lstrip().startswith("#")

def collineate(lines):
	ret = []
	current_line = ""
	depth = 0
	for line in lines:
		if is_line_ignored(line) and not is_partial_comment(line) and not is_partial_comment(current_line) and not is_partial_comment(current_line + line):
			if current_line != "":
				ret.append(current_line)
				current_line = ""
			for c in line:
				if c == "{":
					depth += 1
				elif c == "}":
					if depth == 0:
						sys.exit("Too many close braces on line:\n{}".format(line))
					depth -= 1
			ret.append(line)
			continue
		current_line = current_line + line
		for c in line:
			if c == "{":
				depth += 1
			elif c == "}":
				if depth == 0:
					sys.exit("Too many close braces on line:\n{}".format(line))
				depth -= 1
		if depth == 0 and not is_partial_comment(current_line):
			if current_line != "":
				ret.append(current_line)
				current_line = ""
	if current_line != "":
		ret.append(current_line)
	return ret

assert(collineate(["/*", "...", "*/"]) == ["/*...*/"])
assert(collineate(["{", "/*", "*/", "}"]) == ["{/**/}"])
assert(collineate(["{", "{", "}", "}"]) == ["{{}}"])
assert(collineate(["{", "}", "{", "}"]) == ["{}", "{}"])

def pack(line):
	if is_line_ignored(line):
		return line

	ret = ""
	in_leading_whitespace = True
	for c in line:
		if (c == " ") or (c == "\t"):
			if in_leading_whitespace:
				ret = ret + c
		else:
			in_leading_whitespace = False
			ret = ret + c
	return ret

def prettify(line):
	if is_line_ignored(line):
		return line

	ret = ""
	for c in line:
		if (c == ","):
			ret = ret + ", "
		elif (c == "="):
			ret = ret + " = "
		elif (c == "{"):
			ret = ret + "{ "
		elif (c == "}"):
			ret = ret + " }"
		else:
			ret = ret + c
	return ret

def has_an_offset(c):
	return (c == "{") or (c == ",")

def get_offsets(line):
	if is_line_ignored(line):
		return None
	if not braces_match(line):
		return None
	if "{" not in line:
		return None
	ret = []
	offset = -1
	for c in line:
		offset = offset + 1
		if has_an_offset(c):
			ret.append(offset)
			offset = 0
	return ret

def check_offsets(lines, offsets_list):
	return

def collect_max_offsets(offsets_list):
	max_offsets = None
	for offsets in offsets_list:
		if offsets == None:
			continue
		if max_offsets == None:
			max_offsets = list(offsets)
			continue
		if len(offsets) > len(max_offsets):
			max_offsets.extend(offsets[len(max_offsets):])
		for index in range(len(offsets)):
			if max_offsets[index] < offsets[index]:
				max_offsets[index] = offsets[index]
	return max_offsets

def fix_offsets(line, target_offsets):
	if is_line_ignored(line):
		return line
	if target_offsets is None:
		return line
	if not braces_match(line):
		return line
	if "{" not in line:
		return line
	ret = ""
	offset_index = 0
	offset = -1
	for c in line:
		offset = offset + 1
		ret = ret + c
		if has_an_offset(c):
			if offset_index < len(target_offsets):
				ret = ret + " " * (target_offsets[offset_index] - offset)
			offset_index = offset_index + 1
			offset = 0
	return ret

def align_section(lines, args):
	has_pp = any(l.lstrip().startswith("#") for l in lines)
	if has_pp:
		lines = collineate(lines)
		fixed = []
		for l in lines:
			stripped = l.lstrip()
			if stripped.startswith("."):
				content = prettify(pack(stripped)).rstrip()
				fixed.append("\t    " + content)
			elif stripped == "}," or stripped == "}":
				if stripped == "},":
					fixed.append("\t},")
				else:
					fixed.append("\t}")
			else:
				fixed.append(l)
		return fixed
	lines = collineate(lines)
	lines = [pack(l) for l in lines]
	lines = [prettify(l) for l in lines]
	lines = [l.rstrip() for l in lines]
	offsets_list = [get_offsets(l) for l in lines]
	target_offsets = collect_max_offsets(offsets_list)
	if target_offsets is None:
		return lines
	lines = [fix_offsets(l, target_offsets) for l in lines]
	return lines

def align_file(lines, args):
	ret = []
	lines_to_align = None

	for line in lines:
		if lines_to_align is None:
			if "BEGIN ALIGNED SECTION" in line:
				lines_to_align = []
			ret.append(line)
		else:
			if "END ALIGNED SECTION" in line:
				ret = ret + align_section(lines_to_align, args)
				lines_to_align = None
				ret.append(line)
			else:
				lines_to_align.append(line)

	if lines_to_align is not None:
		sys.exit("Incomplete aligned section")

	return ret

if __name__ == '__main__':
	args = parse_args()
	lines = []

	if args.file:
		with open(args.file) as fd:
			lines = [l.rstrip("\n") for l in fd.readlines()]
	else:
		lines = [l.rstrip("\n") for l in sys.stdin.readlines()]

	for line in align_file(lines, args):
		print(line)
