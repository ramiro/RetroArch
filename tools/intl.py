# intl.py

import fnmatch
import logging
import os
import pprint
import sys

import polib


TRANSLATION_FUNCTION = 'msg_hash_to_str'


def enumerate_files():
    top = '.'
    matches = []
    for root, dirnames, filenames in os.walk(top):
        if root == top and 'intl' in dirnames:
            ndx = dirnames.index('intl')
            del dirnames[ndx]
        # print root, dirnames, filenames
        for filename in fnmatch.filter(filenames, '*.c'):
            matches.append(os.path.join(root, filename))
    return matches


def extract_symbols(symbols, filename, file_object, found_list, search_text):
    file_object.seek(0)
    contents = file_object.read()
    lines = contents.splitlines()
    for y, x in found_list:
        line = lines[y][:]
        temp_lineno = y
        begin = x + len(search_text)
        found_opening_parens = False
        while True:
            if begin == len(line):
                temp_lineno += 1
                line += lines[temp_lineno][:]
            if line[begin] not in (' ', '\t'):
                if line[begin] == '(':
                    found_opening_parens = True
                break
            begin += 1
        if not found_opening_parens:
            continue
        parens_cnt = 1
        end = begin + 1
        while True:
            if end == len(line):
                temp_lineno += 1
                line += lines[temp_lineno][:]
            if line[end] == '(':
                parens_cnt += 1
            elif line[end] == ')':
                parens_cnt -= 1
                if not parens_cnt:
                    break
            end += 1
        if not parens_cnt:
            parens_contents = line[begin:end]
            parens_contents = parens_contents.strip(' \t()')
            if parens_contents.isupper():
                symbols.setdefault(parens_contents, []).append((filename, y))
        else:
            logging.error("%s:%d: Reached end of file and didn't find matching closing parenthesis.", filename, y)


def find_translation_calls_in_file(symbols, filename):
    try:
        with open(filename, 'rU') as f:
            found = []
            for line_number, line in enumerate(f):
                column = line.find(TRANSLATION_FUNCTION)
                if column != -1:
                    found.append((line_number, column))
            if found:
                extract_symbols(symbols, filename, f, found, TRANSLATION_FUNCTION)

    except IOError as e:
        logging.error(e)


def extract_translations_from_msg_hash_us_h():
    filename = os.path.join('.', 'intl', 'msg_hash_us.h')
    try:
        with open(filename, 'rU') as f:
            contents = f.read()
            lines = contents.splitlines()

    except IOError as e:
        logging.error(e)


def main(argv=None):
    if argv is None:
        argv = sys.argv

    filenames = enumerate_files()
    # print filenames
    symbols = {}
    for filename in filenames:
        find_translation_calls_in_file(symbols, filename)
    pprint.pprint(symbols)

    return 0


if __name__ == '__main__':
    sys.exit(main())
