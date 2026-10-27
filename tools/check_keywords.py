#!/usr/bin/env python3
# Checks that the keyword lists basic.c uses to skip its keyword chains
# (builtin_words, statement_words) still cover every keyword those functions
# try. Run after adding a statement or function: python3 tools/check_keywords.py
import re, sys, os

path = os.path.join(os.path.dirname(__file__), '..', 'src', 'basic', 'basic.c')
src = open(path).read()
lines = src.split('\n')

def body(start):
    i = next(k for k, l in enumerate(lines) if l.startswith(start))
    j = next(k for k in range(i + 1, len(lines)) if lines[k] == '}')
    return '\n'.join(lines[i:j + 1])

def needed(text, extra=()):
    words = set()
    for w in re.findall(r'accept(?:_word)?\("([^"]*)"\)', text) + list(extra):
        m = re.match(r'[A-Z][A-Z0-9]*', w)
        if m:
            words.add(m.group(0))
    return words

def listed(name):
    m = re.search(r'static const char \*const ' + name + r'\[\] = \{(.*?)\};', src, re.S)
    return re.findall(r'"([^"]*)"', m.group(1))

ok = True
b = body('static value_t builtin(void) {')
names = re.findall(r'"([^"]*)"', re.search(r'names\[\] = \{(.*?)\};', b, re.S).group(1))
for list_name, want in (('builtin_words', needed(b, names)),
                        ('statement_words', needed(body('static int statement(void) {')))):
    have = listed(list_name)
    for w in sorted(want):
        if not any(w.startswith(h) for h in have):
            print('%s is missing %s' % (list_name, w))
            ok = False
print('keyword lists ok' if ok else 'keyword lists need updating')
sys.exit(0 if ok else 1)
