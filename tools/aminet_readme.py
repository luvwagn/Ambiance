#!/usr/bin/env python3
"""Ambiance.readme (Latin-1, as the archive's readme.txt) made Aminet's
AmbianceMC.readme: plain ASCII (the upload's checker reads UTF-8, and
ASCII reads the same everywhere): (R) and (C) for the marks, accents
dropped; [VERSION] the version given; every line 78 characters or fewer.

  aminet_readme.py VERSION [IN [OUT]]    (IN: Ambiance.readme, OUT: stdout)

Used by make aminet-check and by the release workflows (.github)."""
import sys
import unicodedata


def convert(text, version):
    text = text.replace('®', '(R)').replace('©', '(C)')
    text = unicodedata.normalize('NFKD', text).encode('ascii', 'ignore').decode()
    return text.replace('[VERSION]', version)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    version = sys.argv[1].lstrip('v')
    src = sys.argv[2] if len(sys.argv) > 2 else 'Ambiance.readme'
    out = convert(open(src, encoding='latin-1').read(), version)
    long = [i + 1 for i, line in enumerate(out.split('\n')) if len(line) > 78]
    if long:
        print('::error::readme lines over 78 characters: %s' % long)
        return 1
    if len(sys.argv) > 3:
        open(sys.argv[3], 'w', encoding='ascii', newline='\n').write(out)
    else:
        sys.stdout.write(out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
