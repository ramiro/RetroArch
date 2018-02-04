# intl.py

import fnmatch
import logging
import os
import pprint
import sys

import polib


TRANSLATION_FUNCTION = 'msg_hash_to_str'
TRANSLATION_MACRO = 'MSG_HASH'


class SyntaxError(Exception):
    pass


class SyntaxWarning(Exception):
    pass


class DuplicateLiteral(Exception):
    pass


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


def find_token(y, x, lines, search_text):
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
        raise SyntaxWarning("Didn't find opening parenthesis after '%s'." % search_text, y, lines[y])
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
        return parens_contents
    raise SyntaxError("Reached end of file and didn't find matching closing parenthesis.", y, lines[y])


# def extract_symbols(symbols, filename, file_object, found_list, search_text):
#     file_object.seek(0)
#     contents = file_object.read()
#     lines = contents.splitlines()
#     for y, x in found_list:
#         line = lines[y][:]
#         temp_lineno = y
#         begin = x + len(search_text)
#         found_opening_parens = False
#         while True:
#             if begin == len(line):
#                 temp_lineno += 1
#                 line += lines[temp_lineno][:]
#             if line[begin] not in (' ', '\t'):
#                 if line[begin] == '(':
#                     found_opening_parens = True
#                 break
#             begin += 1
#         if not found_opening_parens:
#             continue
#         parens_cnt = 1
#         end = begin + 1
#         while True:
#             if end == len(line):
#                 temp_lineno += 1
#                 line += lines[temp_lineno][:]
#             if line[end] == '(':
#                 parens_cnt += 1
#             elif line[end] == ')':
#                 parens_cnt -= 1
#                 if not parens_cnt:
#                     break
#             end += 1
#         if not parens_cnt:
#             parens_contents = line[begin:end]
#             parens_contents = parens_contents.strip(' \t()')
#             if parens_contents.isupper():
#                 symbols.setdefault(parens_contents, []).append((filename, y))
#         else:
#             logging.error("%s:%d: Reached end of file and didn't find matching closing parenthesis.", filename, y)


def extract_symbols(symbols, filename, file_object, found_list, search_text):
    file_object.seek(0)
    contents = file_object.read()
    lines = contents.splitlines()
    for y, x in found_list:
        try:
            text = find_token(y, x, lines, search_text)
        except SyntaxWarning:
            continue
        except SyntaxError as e:
            logging.error(e)
            return
        else:
            text = text.strip(' \t()')
            if text.isupper():
                symbols.setdefault(text, []).append((filename, y))


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


def extract_translations(original_literals, filename, file_object, found_list, search_text):
    file_object.seek(0)
    contents = file_object.read()
    lines = contents.splitlines()
    for y, x in found_list:
        try:
            text = find_token(y, x, lines, search_text)
        except SyntaxWarning:
            continue
        except SyntaxError as e:
            logging.error(e)
            return
        else:
            text = text.strip('( \t')
            parts = text.split(',', 1)
            symbol = parts[0].rstrip(' \t')
            if len(parts) > 1:
                literal = parts[1].strip(' \t"')
                if symbol in original_literals:
                    raise DuplicateLiteral(symbol, y)
                original_literals[symbol] = {'lineno': y, 'literal': literal}
            else:
                raise SyntaxWarning("Malformed MSG_HASH call.", filename, y)


def extract_translations_from_msg_hash_xx_h(locale, original_literals):
    filename = os.path.join('.', 'intl', 'msg_hash_%s.h' % locale)
    try:
        with open(filename, 'rU') as f:
            found = []
            for line_number, line in enumerate(f):
                column = line.find(TRANSLATION_MACRO)
                if column != -1:
                    found.append((line_number, column))
            if found:
                extract_translations(original_literals, filename, f, found, TRANSLATION_MACRO)

    except IOError as e:
        logging.error(e)


def main(argv=None):
    if argv is None:
        argv = sys.argv

    filenames = enumerate_files()
    # print filenames
    symbols = {}
    original_literals = {}
    for filename in filenames:
        find_translation_calls_in_file(symbols, filename)
    # pprint.pprint(symbols)
    extract_translations_from_msg_hash_xx_h('us', original_literals)
    # pprint.pprint(original_literals)
    pof = polib.POFile()
    for entry in sorted(original_literals, key=lambda e: original_literals[e]['lineno']):
        # pprint.pprint("%r %r" % (entry, original_literals[entry]))
        symbol_def = symbols.pop(entry, None)
        if symbol_def is not None:
            po_entry = polib.POEntry(
                msgid=polib.unescape(original_literals[entry]['literal']),
                msgstr='',
                occurrences=symbol_def,
                comment=entry,
                flags=['c-format']
            )
            pof.append(po_entry)
    # pof.save('/dev/stdout')
    # print pof.percent_translated()
    # pprint.pprint(symbols)
    return 0


if __name__ == '__main__':
    sys.exit(main())
