# intl.py
from __future__ import print_function

import datetime
import fnmatch
import io
import logging
import os
import pprint  # noqa
import re
import sys
import textwrap
from optparse import OptionParser

import polib

__version__ = '0.1'

TRANSLATION_FUNCTION = 'msg_hash_to_str'
TRANSLATION_MACRO = 'MSG_HASH'

ORIGINAL_LITERAL_FILES = ('lbl', 'us')

RA_LOCALE_NAME_MAP = {
    'pt_br': 'pt_BR',
    'pt_pt': 'pt_PT',
    'us': 'en_US',
    'chs': 'zh_Hans',
    'cht': 'zh_Hant',
}

RE_MULTIPART_LITERAL = re.compile(r'([^\\])"\s+"')

# msg_hash_cht.c: C source, UTF-8 Unicode (with BOM) text
# msg_hash_pl.h: UTF-8 Unicode (with BOM) text
# msg_hash_ko.h: UTF-8 Unicode (with BOM) text
# msg_hash_chs.c: C source, UTF-8 Unicode (with BOM) text
# msg_hash_ja.h: UTF-8 Unicode (with BOM) text
# msg_hash_ar.h: UTF-8 Unicode (with BOM) text
# msg_hash_cht.h: UTF-8 Unicode (with BOM) text
# msg_hash_ja.c: C source, UTF-8 Unicode (with BOM) text
# msg_hash_ko.c: C source, UTF-8 Unicode (with BOM) text
# msg_hash_pl.c: C source, UTF-8 Unicode (with BOM) text
# msg_hash_ar.c: C source, UTF-8 Unicode (with BOM) text
# msg_hash_chs.h: UTF-8 Unicode (with BOM) text
# msg_hash_ru.c: C source, UTF-8 Unicode (with BOM) text
# msg_hash_ru.h: UTF-8 Unicode (with BOM) text
# msg_hash_vn.h: UTF-8 Unicode (with BOM) text
#
# Files which need to be UTF-8 Unicode (with BOM) on disk:
H_FILES_WITH_UTF8_BOM = (
    'pl',
    'ko',
    'ja',
    'ar',
    'cht',
    'chs',
    'ru',
    'vn',
)

PO_FILES_DIR = 'locale'

BASE_PO_METADATA = {
    'Project-Id-Version': 'RetroArch',
    # 'Report-Msgid-Bugs-To': 'you@example.com',
    'Last-Translator': 'RetroArch translators',
    'Language-Team': 'RetroArch translators',
    'MIME-Version': '1.0',
    'Content-Type': 'text/plain; charset=utf-8',
    'Content-Transfer-Encoding': '8bit',
}


class CParseError(Exception):
    pass


class CParseWarning(Exception):
    pass


class SkipOccurrence(Exception):
    pass


class DuplicateLiteral(Exception):
    pass


def enumerate_files():
    top = os.path.curdir
    matches = []
    for root, dirnames, filenames in os.walk(top):
        if root == top and 'intl' in dirnames:
            ndx = dirnames.index('intl')
            del dirnames[ndx]
        # print(root, dirnames, filenames)
        for filename in fnmatch.filter(filenames, '*.c'):
            full_path = os.path.join(root, filename)
            if full_path.startswith(os.path.curdir + os.path.sep):
                full_path = full_path[2:]
            matches.append(full_path)
    return matches


def find_token(y, x, lines, search_text):
    # print('lines = %s' % lines)
    line = lines[y][:]
    temp_lineno = y
    begin = x + len(search_text)
    next_char = True
    found_opening_parens = False
    # while True:
    while temp_lineno < len(lines):
        # print('begin = %d' % begin)
        # print('len(line) ("%s") == %d' % (line, len(line)))
        if begin == len(line):
            # print('begin == len(line)')
            temp_lineno += 1
            # print('temp_lineno = %d' % temp_lineno)
            # print('lines[temp_lineno] = %s' % lines[temp_lineno])
            line += lines[temp_lineno][:]
            if begin == len(line):
                raise CParseWarning("Didn't find opening parenthesis after '%s'." % search_text, y, lines[y])
        elif next_char and line[begin] == '_':
            raise SkipOccurrence
        if line and line[begin] not in (' ', '\t'):
            if line[begin] == '(':
                found_opening_parens = True
            break
        begin += 1
        next_char = False
    if not found_opening_parens:
        raise CParseWarning("Didn't find opening parenthesis after '%s'." % search_text, y, lines[y])
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
    raise CParseError("Reached end of file and didn't find matching closing parenthesis.", y, lines[y])


def extract_symbols(symbols, filename, file_object, found_list, search_text):
    file_object.seek(0)
    contents = file_object.read()
    lines = contents.splitlines()
    for y, x in found_list:
        try:
            text = find_token(y, x, lines, search_text)
        except SkipOccurrence:
            continue
        except CParseWarning as e:
            logging.warn(e)
            continue
        except CParseError as e:
            logging.error(e)
            return
        else:
            text = text.strip(' \t()')
            # TODO: Check what's the RA policy Re:
            # * Casing of these macro names
            # * Which macro name prefixes (i.e. "MSG_", "MENU_ENUM_") actually mark translatable content
            if text.isupper():
                symbols.setdefault(text, []).append((filename, y + 1))  # Our line index is 0-based


def find_translation_calls_in_file(symbols, filename):
    try:
        # TODO: How to best handle inconsistent/undefined encoding of RA source files
        # e.g. wii/libogc/libogc/console_font_8x16.c at offset 89868

        # with io.open(filename, 'rt', encoding='ascii', newline=None) as f:
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


def extract_translations(symbol_defs, filename, file_object, found_list, search_text):
    file_object.seek(0)
    contents = file_object.read()
    lines = contents.splitlines()
    for y, x in found_list:
        try:
            text = find_token(y, x, lines, search_text)
        except CParseWarning:
            continue
        except CParseError as e:
            logging.error(e)
            return
        else:
            text = text.strip('( \t')
            parts = text.split(',', 1)
            symbol = parts[0].rstrip(' \t')
            if len(parts) > 1:
                if symbol in symbol_defs:
                    raise DuplicateLiteral(symbol, y)
                raw_literal = parts[1].strip(' \t')
                literal_parts = RE_MULTIPART_LITERAL.split(raw_literal)
                literal = u''.join(literal_parts)
                if literal[0] == '"' and literal[-1] == '"':
                     literal = literal[1:-1]
                symbol_defs[symbol] = {
                    'file': filename,
                    'lineno': y + 1,  # Our line index is 0-based
                    'literal': literal
                }
            else:
                raise CParseWarning("Malformed MSG_HASH call.", filename, y + 1)


def extract_translations_from_msg_hash_xx_h(locale):
    symbol_defs = {}
    filename = os.path.join('.', 'intl', 'msg_hash_%s.h' % locale)
    try:
        # with open(filename, 'rU') as f:
        with io.open(filename, 'rt', encoding='utf-8', newline=None) as f:
            found = []
            for line_number, line in enumerate(f):
                column = line.find(TRANSLATION_MACRO)
                if column != -1:
                    found.append((line_number, column))
            if found:
                extract_translations(symbol_defs, filename, f, found, TRANSLATION_MACRO)

    except IOError as e:
        logging.error(e)
    return symbol_defs


def common(symbols, english_symdefs):
    filenames = enumerate_files()
    # print(filenames)
    for filename in filenames:
        find_translation_calls_in_file(symbols, filename)
    # pprint.pprint(symbols)
    for orig_file in ORIGINAL_LITERAL_FILES:
        english_symdefs.update(extract_translations_from_msg_hash_xx_h(orig_file))
    # pprint.pprint(english_symdefs)


def check(options):

    def key(entry):
        return (english_symdefs[entry]['file'], english_symdefs[entry]['lineno'])

    if options.output_file:
        logging.error("check action doesn't need the -o/--output option")
        return 2
    symbols = {}
    english_symdefs = {}
    common(symbols, english_symdefs)
    for entry in sorted(english_symdefs, key=key):
        symbols.pop(entry, None)
    if symbols:
        logging.warning("The following RetroArch translatable literal IDs don't have an English original literal defined:")
        for k in symbols:
            logging.warning("\t%s (used in %s)", k, ', '.join('%s:%d' % info for info in symbols[k]))
    return 0


def h2po(options):

    def key(entry):
        return (english_symdefs[entry]['file'], english_symdefs[entry]['lineno'])

    locale = RA_LOCALE_NAME_MAP.get(options.locale, options.locale)
    if options.output_file == '-':
        output_file = 'CON' if sys.platform == 'win32' else '/dev/stdout'
    else:
        if not options.output_file:
            output_file = os.path.join(PO_FILES_DIR, '%s.po' % locale)
        else:
            output_file = options.output_file
        if os.path.exists(output_file) and not options.force:
            logging.critical("%s exists. Refusing to overwite it", output_file)
            return 3

    symbols = {}
    english_symdefs = {}
    common(symbols, english_symdefs)
    # TODO: Pass wrapwidth=160?
    pof = polib.POFile()
    utcnow = datetime.datetime.utcnow().replace(second=0, microsecond=0).isoformat(' ') + '+0000'
    pof.metadata = dict(BASE_PO_METADATA)
    pof.metadata.update({
        'Language': locale,
    })
    existing_translations = {}
    if locale == 'en_US':
        pof.metadata.update({
            'POT-Creation-Date': utcnow,
        })
    else:
        pof.metadata.update({
            'PO-Revision-Date': utcnow,
        })
        existing_translations = extract_translations_from_msg_hash_xx_h(options.locale)
        # pprint.pprint(existing_translations)

    for entry in sorted(english_symdefs, key=key):
        msgid = english_symdefs[entry]['literal']
        if locale == 'en_US':
            msgstr = ''
        else:
            msgstr = existing_translations.pop(entry, {}).get('literal', '')
        if not msgstr and msgid.islower() and ' ' not in msgid and '_' in msgid:
            continue
        edata = {
            'msgstr': polib.unescape(msgstr),
            'msgid': polib.unescape(msgid),
            # TODO: Enhance these heuristics
            'flags': ['c-format'] if '%' in msgid else [],
            'msgctxt': entry,
        }
        if locale != 'en_US' and options.interpret_equal_trans_as_empty and msgstr and msgstr == msgid:
            edata['msgstr'] = ''
            edata['comment'] = 'Please review and decide if this translation should actually be equal to the English original and edit it accordingly if needed. IMPORTANT: In any case remove this comment afterwards'
        symbol_usage_list = symbols.get(entry)
        if symbol_usage_list is not None:
            edata['occurrences'] = symbol_usage_list
        po_entry = polib.POEntry(**edata)
        pof.append(po_entry)
    if existing_translations:
        error_text = ['The following RetroArch translatable literal IDs don\'t have an English original literal defined:']
        for k in existing_translations:
            error_text.append('\t%s' % k)
        logging.warning('\n'.join(error_text))
    pof.save(output_file)
    return 0


def updatepo(options):

    def key(entry):
        return (original_literals[entry]['file'], original_literals[entry]['lineno'])

    locale = RA_LOCALE_NAME_MAP.get(options.locale, options.locale)
    po_data = None
    if options.output_file == '-':
        output_file = 'CON' if sys.platform == 'win32' else '/dev/stdout'
    else:
        if not options.output_file:
            output_file = os.path.join(PO_FILES_DIR, '%s.po' % locale)
        else:
            output_file = options.output_file
        if os.path.exists(output_file):
            po_data = polib.pofile(output_file)

    symbols = {}
    original_literals = {}
    common(symbols, original_literals)
    # TODO: Pass wrapwidth=160?
    pof = polib.POFile()
    utcnow = datetime.datetime.utcnow().replace(second=0, microsecond=0).isoformat(' ') + '+0000'
    if locale == 'en_US':
        pof.metadata = dict(BASE_PO_METADATA)
        pof.metadata.update({
            'Language': locale,
            'POT-Creation-Date': utcnow,
        })
        for entry in sorted(original_literals, key=key):
            symbol_def = symbols.get(entry)
            if symbol_def is not None:
                # msgid = polib.unescape(original_literals[entry]['literal'])
                msgid = original_literals[entry]['literal']
                # TODO: Enhance these heuristics
                flags = ['c-format'] if '%' in msgid else []
                po_entry = polib.POEntry(
                    msgid=msgid,
                    msgstr='',
                    occurrences=symbol_def,
                    # comment=entry,
                    msgctxt=entry,
                    flags=flags
                )
                pof.append(po_entry)
        pof.save(output_file)
    else:
        refpot_file = os.path.join(PO_FILES_DIR, 'en_US.po')
        refpot = polib.pofile(refpot_file)
        if po_data is not None:
            po_data.merge(refpot)
            po_data.metadata.update({
                'Language': locale,
                'PO-Revision-Date': utcnow,
            })
            po_data.save(output_file)
    return 0


def po2h(options):
    locale = RA_LOCALE_NAME_MAP.get(options.locale, options.locale)
    if not options.input_file:
        input_file = os.path.join(PO_FILES_DIR, '%s.po' % locale)
    else:
        input_file = options.input_file
    if not os.path.exists(input_file):
        logging.critical("Input file %s doesn't exist.", input_file)
        return 1
    po_data = polib.pofile(input_file)
    if not options.output_file:
        output_file = os.path.join('.', 'intl', 'msg_hash_%s.h' % options.locale)
    else:
        output_file = options.output_file
    enc = 'utf-8-sig' if options.locale in H_FILES_WITH_UTF8_BOM else 'utf-8'
    with io.open(output_file, 'w', encoding=enc) as f:
        f.write(u'/* This file is auto-generated. Your changes will be overwritten. */\n\n')
        for entry in po_data:
            trans = entry.msgstr if entry.translated() else entry.msgid
            h_entry = textwrap.dedent("""\
                MSG_HASH(
                \t%s,
                \t"%s"
                \t)
                """) % (entry.msgctxt, polib.escape(trans))
            f.write(h_entry)
    return 0


def main(argv=None):
    if argv is None:
        argv = sys.argv

    parser = OptionParser(
        usage='%prog [options] [action]',
        version='%prog ' + __version__
    )
    parser.add_option('-l', '--locale', default='us', help='locale name to work with')
    parser.add_option('-o', '--output', dest='output_file', help='PO file to write to')
    parser.add_option('-f', '--force', action='store_true', help='Force overwriting extisting PO file when using h2po action')
    parser.add_option('-e', dest='interpret_equal_trans_as_empty', action='store_true', help='When a translation in the .h files is equal to its English original store it as untranslated (empty) in the PO file.')
    parser.add_option('-i', '--input', dest='input_file', help='PO file to read from (po2h action)')
    options, args = parser.parse_args(args=argv[1:])
    if not args:
        action = 'check'
    elif len(args) == 1:
        action = args[0]
    else:
        parser.error('extraneous command line options')
    if action == 'check':
        return check(options)
    elif action == 'h2po':
        return h2po(options)
    elif action == 'updatepo':
        return updatepo(options)
    elif action == 'po2h':
        return po2h(options)
    else:
        parser.error('unknown action: \'%s\'' % action)


if __name__ == '__main__':
    logging.basicConfig(format='%(message)s')
    sys.exit(main())
