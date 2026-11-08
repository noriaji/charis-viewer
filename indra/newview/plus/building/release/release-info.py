#!/usr/bin/env python3
"""Prints one fact about the release in this checkout, for the release workflow.

  python -I release-info.py version          the version of plus/version/version.h
                                             as in file names (0.1.0-alpha1)
  python -I release-info.py tag-version TAG  the version in the asset names for a
                                             release tag (v0.1.0-alpha1 -> 0.1.0-alpha1);
                                             stops when the tag's form is wrong or its
                                             major.minor.patch is not version.h's
  python -I release-info.py project-url      PROJECT_URL of plus/independence/independence.h
                                             (the repository the update feed points at)

Exit status: 0 = printed, 1 = stopped (the reason is printed), 2 = wrong arguments.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True     # no __pycache__ in the source tree
from releasecommon import SOURCE, independence_constant, release_version, tag_version  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    sub = ap.add_subparsers(dest='what', required=True)
    sub.add_parser('version')
    sub.add_parser('tag-version').add_argument('tag')
    sub.add_parser('project-url')
    a = ap.parse_args()
    try:
        if a.what == 'version':
            print(release_version(SOURCE)[0])
        elif a.what == 'tag-version':
            print(tag_version(a.tag, SOURCE))
        else:
            url = independence_constant(SOURCE, 'PROJECT_URL')
            if not url:
                raise ValueError('PROJECT_URL is not once in plus/independence/independence.h')
            print(url)
    except (ValueError, OSError) as e:
        print('release-info: %s' % e, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
