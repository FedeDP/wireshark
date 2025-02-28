#!/usr/bin/env python3
#
# html2text.py - converts HTML to text
#
# Wireshark - Network traffic analyzer
# By Gerald Combs <gerald@wireshark.org>
# Copyright 1998 Gerald Combs
#
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import unicode_literals

__author__      = "Peter Wu <peter@lekensteyn.nl>"
__copyright__   = "Copyright 2015, Peter Wu"
__license__     = "GPL (v2 or later)"

# TODO:
#   multiple list indentation levels (modify bullets?)
#   maybe allow for ascii output instead of utf-8?

import sys
from textwrap import TextWrapper
try:
    from HTMLParser import HTMLParser
    from htmlentitydefs import name2codepoint
except ImportError: # Python 3
    from html.parser import HTMLParser
    from html.entities import name2codepoint
    unichr = chr # for html entity handling

class TextHTMLParser(HTMLParser):
    """Converts a HTML document to text."""
    def __init__(self):
        try:
            # Python 3.4
            HTMLParser. __init__(self, convert_charrefs=True)
        except Exception:
            HTMLParser. __init__(self)
        # All text, concatenated
        self.output_buffer = ''
        # The current text block which is being constructed
        self.text_block = ''
        # Whether the previous element was terminated with whitespace
        self.need_space = False
        # Whether to prevent word-wrapping the contents (for "pre" tag)
        self.skip_wrap = False
        # Quoting
        self.need_quote = False
        self.quote_stack = []
        # track list items
        self.list_item_prefix = None
        self.ordered_list_index = None
        self.stack_list_item_prefix = []
        self.stack_ordered_list_index = []
        self.list_indent_level = 0
        self.list_item_indent = ""
        # Indentation (for heading and paragraphs)
        self.indent_levels = [0, 0]
        # Don't dump CSS, scripts, etc.
        self.ignore_tags = ('head', 'style', 'script')
        self.ignore_level = 0
        # href footnotes.
        self.footnotes = []
        self.href = None

    def _wrap_text(self, text):
        """Wraps text, but additionally indent list items."""
        initial_indent = indent = sum(self.indent_levels) * ' '
        if self.list_item_prefix:
            initial_indent += self.list_item_prefix
            indent += '    '
        kwargs = {
            'width': 72,
            'initial_indent': initial_indent,
            'subsequent_indent': indent
        }
        kwargs['break_on_hyphens'] = False
        wrapper = TextWrapper(**kwargs)
        return '\n'.join(wrapper.wrap(text))

    def _commit_block(self, newline='\n\n'):
        text = self.text_block
        if text:
            if not self.skip_wrap:
                text = self._wrap_text(text)
            self.output_buffer += text + newline
            self.text_block = ''
        self.need_space = False

    def handle_starttag(self, tag, attrs):
        # end a block of text on <br>, but also flush list items which are not
        # terminated.
        if tag == 'br' or tag == 'li':
            self._commit_block('\n')
        if tag == 'code':
            self.need_quote = True
            self.quote_stack.append('`')
        if tag == 'pre':
            self.skip_wrap = True
        if tag in ('ol', 'ul'):
            self.list_indent_level += 1
            self.list_item_indent = "   " * (self.list_indent_level - 1)
            self.stack_ordered_list_index.append(self.ordered_list_index)
            self.stack_list_item_prefix.append(self.list_item_prefix)
        # Following list items are numbered.
        if tag == 'ol':
            self.ordered_list_index = 1
        if tag == 'ul':
            self.list_item_prefix = self.list_item_indent + '  • '
        if tag == 'li' and self.ordered_list_index:
            self.list_item_prefix =  self.list_item_indent + ' %d. ' % (self.ordered_list_index)
            self.ordered_list_index += 1
        if tag[0] == 'h' and len(tag) == 2 and \
            (tag[1] >= '1' and tag[1] <= '6'):
            self.indent_levels = [int(tag[1]) - 1, 0]
        if tag == 'p':
            self.indent_levels[1] = 1
        if tag == 'a':
            try:
                href = [attr[1] for attr in attrs if attr[0] == 'href'][0]
                if '://' in href: # Skip relative URLs and links.
                    self.href = href
            except IndexError:
                self.href = None
        if tag == 'span':
            try:
                el_class = [attr[1] for attr in attrs if attr[0] == 'class'][0]
                if 'menuseq' in el_class:
                    sys.stderr.write('menuseq\n')
                    self.need_quote = True
                    self.quote_stack.append('"')
            except IndexError:
                pass
        if tag in self.ignore_tags:
            self.ignore_level += 1

    def handle_data(self, data):
        quote = ''
        if self.need_quote:
            quote = self.quote_stack[-1]
        if self.ignore_level > 0:
            return
        elif self.skip_wrap:
            block = data
        else:
            if self.href and data == self.href:
                # This is a self link. Don't create a footnote.
                self.href = None

            # For normal text, fold multiple whitespace and strip
            # leading and trailing spaces for the whole block (but
            # keep spaces in the middle).
            block = quote
            if data.strip() and data[:1].isspace():
                # Keep spaces in the middle
                self.need_space = True
            if self.need_space and data.strip() and self.text_block:
                block = ' ' + quote
            block += ' '.join(data.split())
            self.need_space = data[-1:].isspace()
        self.text_block += block
        self.need_quote = False

    def handle_endtag(self, tag):
        block_elements = 'p li ul pre ol h1 h2 h3 h4 h5 h6'
        #block_elements += ' dl dd dt'
        if tag in block_elements.split():
            self._commit_block()
        if tag in ('code', 'span'):
            # XXX This span isn't guaranteed to match its opening.
            self.text_block += self.quote_stack.pop()
        if tag in ('ol', 'ul'):
            self.list_indent_level -= 1
            self.list_item_indent = "   " * (self.list_indent_level - 1)
            self.ordered_list_index = self.stack_ordered_list_index.pop()
            self.list_item_prefix = self.stack_list_item_prefix.pop()
        if tag == 'pre':
            self.skip_wrap = False
        if tag == 'a' and self.href:
            self.footnotes.append(self.href)
            self.text_block += '[{0}]'.format(len(self.footnotes))
        if tag in self.ignore_tags:
            self.ignore_level -= 1

    def handle_charref(self, name):
        self.handle_data(unichr(int(name)))

    def handle_entityref(self, name):
        self.handle_data(unichr(name2codepoint[name]))

    def close(self):
        HTMLParser.close(self)
        self._commit_block()

        if len(self.footnotes) > 0:
            self.list_item_prefix = None
            self.indent_levels = [1, 0]
            self.text_block = 'References'
            self._commit_block()
            self.indent_levels = [1, 1]
            footnote_num = 1
            for href in self.footnotes:
                self.text_block += '{0:>2}. {1}\n'.format(footnote_num, href)
                footnote_num += 1
                self._commit_block('\n')


        byte_output = self.output_buffer.encode('utf-8')
        if hasattr(sys.stdout, 'buffer'):
            sys.stdout.buffer.write(byte_output)
        else:
            sys.stdout.write(byte_output)


def main():
    htmlparser = TextHTMLParser()
    if len(sys.argv) > 1 and sys.argv[1] != '-':
        filename = sys.argv[1]
        f = open(filename, 'rb')
    else:
        filename = None
        f = sys.stdin
    try:
        if hasattr(f, 'buffer'):
            # Access raw (byte) buffer in Python 3 instead of decoded one
            f = f.buffer
        # Read stdin as as Unicode string
        htmlparser.feed(f.read().decode('utf-8'))
    finally:
        if filename is not None:
            f.close()
    htmlparser.close()

if __name__ == '__main__':
    sys.exit(main())
