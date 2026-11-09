#!/usr/bin/env python3
"""\
@file viewer_manifest.py
@author Ryan Williams
@brief Description of all installer viewer files, and methods for packaging
       them into installers for all supported platforms.

$LicenseInfo:firstyear=2006&license=viewerlgpl$
Second Life Viewer Source Code
Copyright (C) 2006-2014, Linden Research, Inc.

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation;
version 2.1 of the License only.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
$/LicenseInfo$
"""
import errno
import glob
import itertools
import json
import os
import os.path
import plistlib
import random
import re
import secrets
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time

viewer_dir = os.path.dirname(__file__)
# Add indra/lib/python to our path so we don't have to muck with PYTHONPATH.
# Put it FIRST because some of our build hosts have an ancient install of
# indra.util.llmanifest under their system Python!
sys.path.insert(0, os.path.join(viewer_dir, os.pardir, "lib", "python"))
from indra.util.llmanifest import LLManifest, main, path_ancestors, CHANNEL_VENDOR_BASE, RELEASE_CHANNEL, ManifestError, MissingError
import llsd

class ViewerManifest(LLManifest):
    def is_packaging_viewer(self):
        # Some commands, files will only be included
        # if we are packaging the viewer on windows.
        # This manifest is also used to copy
        # files during the build (see copy_w_viewer_manifest
        # and copy_l_viewer_manifest targets)
        return 'package' in self.args['actions']

    def construct(self):
        super(ViewerManifest, self).construct()
        self.path(src="../../scripts/messages/message_template.msg", dst="app_settings/message_template.msg")

        # ... and the lsl/lua script definition files (needed at build time and packaging)
        pkgdir = os.path.join(self.args['build'], os.pardir, 'packages')
        with self.prefix(src=os.path.join(pkgdir, 'lsl_definitions'), dst="app_settings/syntax_default"):
            self.path("*.txt")
            self.path("*.yaml")
            self.path("*.xml")
            self.path("*.json")
            self.path("*.luau")
            self.path("*.yml")

        if self.is_packaging_viewer():
            with self.prefix(src_dst="app_settings"):
                self.exclude("logcontrol.xml")
                self.exclude("logcontrol-dev.xml")
                self.path("*.ini")
                self.path("*.txt")
                self.path("*.xml")

                # include the entire shaders directory recursively
                self.path("shaders")
                # include the extracted list of contributors
                contributions_path = os.path.join(self.args['source'], "..", "..", "doc", "contributions.txt")
                contributor_names = self.extract_names(contributions_path)
                self.put_in_file(contributor_names.encode(), "contributors.txt", src=contributions_path)

                # ... and the default camera position settings
                self.path("camera")

                # ... and the entire windlight directory
                self.path("windlight")

                # ... and the entire image filters directory
                self.path("filters")

                # <plus> data files of features ported from other viewers (indra/newview/plus/)
                self.path("plus")
                # </plus>

                # <plus> shader bundle (research/159 §3.3): made by
                # tools/pack-shader-bundle.py straight into the build output
                # folder (not tracked in git), so take it from there; src is dst
                # then and the copy step skips it. Optional: a developer build
                # has none, tools/make-release-zip.sh stops a release without
                # it. pipeline_keys_seed.bin is for later (research/159 P3).
                for bundle_file in ("shaders.vkb", "pipeline_keys_seed.bin"):
                    bundle_rel = os.path.join("vk_shader_bundle", bundle_file)
                    self.path_optional(self.dst_path_of(bundle_rel), bundle_rel)
                # </plus>

                # ... and the included spell checking dictionaries
                pkgdir = os.path.join(self.args['build'], os.pardir, 'packages')
                with self.prefix(src=pkgdir):
                    self.path("dictionaries")

                # include the extracted packages information
                self.path(src=os.path.join(self.args['build'],"packages-info.txt"), dst="packages-info.txt")
                # CHOP-955: If we have "sourceid" or "viewer_channel" in the
                # build process environment, generate it into
                # settings_install.xml.
                settings_template = dict(
                    sourceid=dict(Comment='Identify referring agency to Linden web servers',
                                  Persist=1,
                                  Type='String',
                                  Value=''),
                    CmdLineGridChoice=dict(Comment='Default grid',
                                  Persist=0,
                                  Type='String',
                                  Value=''),
                    CmdLineChannel=dict(Comment='Command line specified channel name',
                                        Persist=0,
                                        Type='String',
                                        Value=''))
                settings_install = {}
                sourceid = self.args.get('sourceid')
                if sourceid:
                    settings_install['sourceid'] = settings_template['sourceid'].copy()
                    settings_install['sourceid']['Value'] = sourceid
                    print("Set sourceid in settings_install.xml to '%s'" % sourceid)

                if self.args.get('channel_suffix'):
                    settings_install['CmdLineChannel'] = settings_template['CmdLineChannel'].copy()
                    settings_install['CmdLineChannel']['Value'] = self.channel_with_pkg_suffix()
                    print("Set CmdLineChannel in settings_install.xml to '%s'" % self.channel_with_pkg_suffix())

                if self.args.get('grid'):
                    settings_install['CmdLineGridChoice'] = settings_template['CmdLineGridChoice'].copy()
                    settings_install['CmdLineGridChoice']['Value'] = self.grid()
                    print("Set CmdLineGridChoice in settings_install.xml to '%s'" % self.grid())

                # put_in_file(src=) need not be an actual pathname; it
                # only needs to be non-empty
                self.put_in_file(llsd.format_pretty_xml(settings_install),
                                 "settings_install.xml",
                                 src="environment")

            # Include the full Luau license in the packaged viewer.
            with self.prefix(src=pkgdir):
                self.path("LICENSES/luau.txt")

            with self.prefix(src_dst="character"):
                self.path("*.llm")
                self.path("*.xml")
                self.path("*.tga")

            # Include our fonts
            with self.prefix(src="../packages/fonts",src_dst="fonts"):
                self.path("*.ttf")
                self.path("*.txt")

            # skins
            with self.prefix(src_dst="skins"):
                    # include the entire textures directory recursively
                    with self.prefix(src_dst="*/textures"):
                            self.path("*/*.jpg")
                            self.path("*/*.png")
                            self.path("*.tga")
                            self.path("*.j2c")
                            self.path("*.png")
                            self.path("textures.xml")
                    self.path("*/xui/*/*.xml")
                    self.path("*/xui/*/widgets/*.xml")
                    self.path("*/*.xml")

                    # Update: 2017-11-01 CP Now we store app code in the html folder
                    #         Initially the HTML/JS code to render equirectangular
                    #         images for the 360 capture feature but more to follow.
                    with self.prefix(src="*/html", dst="*/html"):
                        self.path("*/*/*/*.js")
                        self.path("*/*/*.html")

            #build_data.json.  Standard with exception handling is fine.  If we can't open a new file for writing, we have worse problems
            #platform is computed above with other arg parsing
            build_data_dict = {"Type":"viewer","Version":'.'.join(self.args['version']),
                            "Channel Base": CHANNEL_VENDOR_BASE,
                            "Channel":self.channel_with_pkg_suffix(),
                            "Platform":self.build_data_json_platform,
                            "Address Size":self.address_size,
                            "Update Service":"https://update.secondlife.com/update",
                            }
            # Only store this if it's both present and non-empty
            bugsplat_db = self.args.get('bugsplat')
            if bugsplat_db:
                build_data_dict["BugSplat DB"] = bugsplat_db
            build_data_dict = self.finish_build_data_dict(build_data_dict)
            with open(os.path.join(os.pardir,'build_data.json'), 'w') as build_data_handle:
                json.dump(build_data_dict,build_data_handle)

            #we likely no longer need the test, since we will throw an exception above, but belt and suspenders and we get the
            #return code for free.
            if not self.path2basename(os.pardir, "build_data.json"):
                print("No build_data.json file")

    def finish_build_data_dict(self, build_data_dict):
        return build_data_dict

    def grid(self):
        return self.args['grid']

    def channel(self):
        return self.args['channel']

    def channel_with_pkg_suffix(self):
        fullchannel=self.channel()
        channel_suffix = self.args.get('channel_suffix')
        if channel_suffix:
            fullchannel+=' '+channel_suffix
        return fullchannel

    def channel_variant(self):
        global CHANNEL_VENDOR_BASE
        return self.channel().replace(CHANNEL_VENDOR_BASE, "").strip()

    def channel_type(self): # returns 'release', 'beta', 'project', or 'test'
        channel_qualifier=self.channel_variant().lower()
        if channel_qualifier.startswith('release'):
            channel_type='release'
        elif channel_qualifier.startswith('beta'):
            channel_type='beta'
        elif channel_qualifier.startswith('project'):
            channel_type='project'
        else:
            channel_type='test'
        return channel_type

    def channel_variant_app_suffix(self):
        # get any part of the channel name after the CHANNEL_VENDOR_BASE
        suffix=self.channel_variant()
        # by ancient convention, we don't use Release in the app name
        if self.channel_type() == 'release':
            suffix=suffix.replace('Release', '').strip()
        # for the base release viewer, suffix will now be null - for any other, append what remains
        if suffix:
            suffix = "_".join([''] + suffix.split())
        # the additional_packages mechanism adds more to the installer name (but not to the app name itself)
        # ''.split() produces empty list, so suffix only changes if
        # channel_suffix is non-empty
        suffix = "_".join([suffix] + self.args.get('channel_suffix', '').split())
        return suffix

    def installer_base_name(self):
        global CHANNEL_VENDOR_BASE
        # a standard map of strings for replacing in the templates
        substitution_strings = {
            'channel_vendor_base' : '_'.join(CHANNEL_VENDOR_BASE.split()),
            'channel_variant_underscores':self.channel_variant_app_suffix(),
            'version_underscores' : '_'.join(self.args['version']),
            'arch':self.args['arch']
            }
        return "%(channel_vendor_base)s%(channel_variant_underscores)s_%(version_underscores)s_%(arch)s" % substitution_strings

    def installer_base_name_mac(self):
        global CHANNEL_VENDOR_BASE
        # a standard map of strings for replacing in the templates
        substitution_strings = {
            'channel_vendor_base' : '_'.join(CHANNEL_VENDOR_BASE.split()),
            'channel_variant_underscores':self.channel_variant_app_suffix(),
            'version_underscores' : '_'.join(self.args['version'])
            }
        return "%(channel_vendor_base)s%(channel_variant_underscores)s_%(version_underscores)s_universal" % substitution_strings

    def app_name(self):
        global CHANNEL_VENDOR_BASE
        channel_type=self.channel_type()
        if channel_type == 'release':
            app_suffix='Viewer'
        else:
            app_suffix=self.channel_variant()
        return CHANNEL_VENDOR_BASE + ' ' + app_suffix

    def exec_name(self):
        # <plus> Charis's own exe name, not the official SecondLifeViewer (research/227 F4,
        # research/114 stage 2). The build's target stays secondlife-bin (copied to this name).
        return "CharisViewer"
        # </plus>

    def app_name_oneword(self):
        return ''.join(self.app_name().split())

    def icon_path(self):
        return "icons/" + self.channel_type()

    def extract_names(self,src):
        """Extract contributor names from source file, returns string"""
        try:
            with open(src, 'r') as contrib_file:
                lines = contrib_file.readlines()
        except IOError:
            print("Failed to open '%s'" % src)
            raise

        # All lines up to and including the first blank line are the file header; skip them
        lines.reverse() # so that pop will pull from first to last line
        while not re.match(r"\s*$", lines.pop()) :
            pass # do nothing

        # A line that starts with a non-whitespace character is a name; all others describe contributions, so collect the names
        names = []
        for line in lines :
            if re.match(r"\S", line) :
                names.append(line.rstrip())
        # It's not fair to always put the same people at the head of the list
        random.shuffle(names)
        return ', '.join(names)

    def relsymlinkf(self, src, dst=None, catch=True):
        """
        relsymlinkf() is just like symlinkf(), but instead of requiring the
        caller to pass 'src' as a relative pathname, this method expects 'src'
        to be absolute, and creates a symlink whose target is the relative
        path from 'src' to dirname(dst).
        """
        dstdir, dst = self._symlinkf_prep_dst(src, dst)

        # Determine the relative path starting from the directory containing
        # dst to the intended src.
        src = self.relpath(src, dstdir)

        self._symlinkf(src, dst, catch)
        return dst

    def symlinkf(self, src, dst=None, catch=True):
        """
        Like ln -sf, but uses os.symlink() instead of running ln. This creates
        a symlink at 'dst' that points to 'src' -- see:
        https://docs.python.org/3/library/os.html#os.symlink

        If you omit 'dst', this creates a symlink with basename(src) at
        get_dst_prefix() -- in other words: put a symlink to this pathname
        here at the current dst prefix.

        'src' must specifically be a *relative* symlink. It makes no sense to
        create an absolute symlink pointing to some path on the build machine!

        Also:
        - We prepend 'dst' with the current get_dst_prefix(), so it has similar
          meaning to associated self.path() calls.
        - We ensure that the containing directory os.path.dirname(dst) exists
          before attempting the symlink.

        If you pass catch=False, exceptions will be propagated instead of
        caught.
        """
        dstdir, dst = self._symlinkf_prep_dst(src, dst)
        self._symlinkf(src, dst, catch)
        return dst

    def _symlinkf_prep_dst(self, src, dst):
        # helper for relsymlinkf() and symlinkf()
        if dst is None:
            dst = os.path.basename(src)
        dst = os.path.join(self.get_dst_prefix(), dst)
        # Seems silly to prepend get_dst_prefix() to dst only to call
        # os.path.dirname() on it again, but this works even when the passed
        # 'dst' is itself a pathname.
        dstdir = os.path.dirname(dst)
        self.cmakedirs(dstdir)
        return (dstdir, dst)

    def _symlinkf(self, src, dst, catch):
        # helper for relsymlinkf() and symlinkf()
        # the passed src must be relative
        if os.path.isabs(src):
            raise ManifestError("Do not symlinkf(absolute %r, asis=True)" % src)

        # The outer catch is the one that reports failure even after attempted
        # recovery.
        try:
            # At the inner layer, recovery may be possible.
            try:
                os.symlink(src, dst)
            except OSError as err:
                if err.errno != errno.EEXIST:
                    raise
                # We could just blithely attempt to remove and recreate the target
                # file, but that strategy doesn't work so well if we don't have
                # permissions to remove it. Check to see if it's already the
                # symlink we want, which is the usual reason for EEXIST.
                elif os.path.islink(dst):
                    if os.readlink(dst) == src:
                        # the requested link already exists
                        pass
                    else:
                        # dst is the wrong symlink; attempt to remove and recreate it
                        os.remove(dst)
                        os.symlink(src, dst)
                elif os.path.isdir(dst):
                    print("Requested symlink (%s) exists but is a directory; replacing" % dst)
                    shutil.rmtree(dst)
                    os.symlink(src, dst)
                elif os.path.exists(dst):
                    print("Requested symlink (%s) exists but is a file; replacing" % dst)
                    os.remove(dst)
                    os.symlink(src, dst)
                else:
                    # out of ideas
                    raise
        except Exception as err:
            # report
            print("Can't symlink %r -> %r: %s: %s" % \
                  (dst, src, err.__class__.__name__, err))
            # if caller asked us not to catch, re-raise this exception
            if not catch:
                raise

    def relpath(self, path, base=None, symlink=False):
        """
        Return the relative path from 'base' to the passed 'path'. If base is
        omitted, self.get_dst_prefix() is assumed. In other words: make a
        same-name symlink to this path right here in the current dest prefix.

        Normally we resolve symlinks. To retain symlinks, pass symlink=True.
        """
        if base is None:
            base = self.get_dst_prefix()

        # Since we use os.path.relpath() for this, which is purely textual, we
        # must ensure that both pathnames are absolute.
        if symlink:
            # symlink=True means: we know path is (or indirects through) a
            # symlink, don't resolve, we want to use the symlink.
            abspath = os.path.abspath
        else:
            # symlink=False means to resolve any symlinks we may find
            abspath = os.path.realpath

        return os.path.relpath(abspath(path), abspath(base))

    def set_github_output_path(self, variable, path):
        self.set_github_output(variable,
                               os.path.normpath(os.path.join(self.get_dst_prefix(), path)))

    def set_github_output(self, variable, *values):
        GITHUB_OUTPUT = os.getenv('GITHUB_OUTPUT')
        if GITHUB_OUTPUT and values:
            with open(GITHUB_OUTPUT, 'a') as outf:
                if len(values) == 1:
                    print('='.join((variable, values[0])), file=outf)
                else:
                    delim = secrets.token_hex(8)
                    print('<<'.join((variable, delim)), file=outf)
                    for value in values:
                        print(value, file=outf)
                    print(delim, file=outf)

    # <plus> Charis Viewer: the version the installer and the update packages carry
    def charis_version(self):
        """(packVersion, version.h's info) from plus/version/packversion.py: Charis's own
        version (plus/version/version.h) and build number ("0.1.0-alpha.1.3245", "0.1.0" for
        a release), never the official VIEWER_VERSION.txt that self.args['version'] starts
        with (its fourth part is Charis's build number: VIEWER_VERSION_REVISION is
        CHARIS_BUILD_NUMBER, cmake/BuildVersion.cmake). info['product_name'] is the name
        shown in front of the version ("Charis Viewer")."""
        import importlib.util
        folder = os.path.join(self.get_src_prefix(), 'plus', 'version')
        spec = importlib.util.spec_from_file_location('charis_packversion', os.path.join(folder, 'packversion.py'))
        packversion = importlib.util.module_from_spec(spec)
        keep = sys.dont_write_bytecode
        sys.dont_write_bytecode = True      # no __pycache__ in the source tree
        try:
            spec.loader.exec_module(packversion)
        finally:
            sys.dont_write_bytecode = keep
        version = self.args['version']
        try:
            info = packversion.read_version_h(os.path.join(folder, 'version.h'))
            build = packversion.build_number(version[3] if len(version) > 3 else None)
            return packversion.pack_version(info, build), info
        except packversion.VersionError as e:
            raise ManifestError(str(e))
    # </plus>


class Windows_x86_64_Manifest(ViewerManifest):
    # We want the platform, per se, for every Windows build to be 'win'. The
    # VMP will concatenate that with the address_size.
    build_data_json_platform = 'win'
    address_size = 64

    def final_exe(self):
        return self.exec_name()+".exe"

    def finish_build_data_dict(self, build_data_dict):
        build_data_dict['Executable'] = self.final_exe()
        build_data_dict['AppName']    = self.app_name()
        return build_data_dict

    def test_msvcrt_and_copy_action(self, src, dst):
        # This is used to test a dll manifest.
        # It is used as a temporary override during the construct method
        from test_win32_manifest import test_assembly_binding
        # TODO: This is redundant with LLManifest.copy_action(). Why aren't we
        # calling copy_action() in conjunction with test_assembly_binding()?
        if src and (os.path.exists(src) or os.path.islink(src)):
            # ensure that destination path exists
            self.cmakedirs(os.path.dirname(dst))
            self.created_paths.append(dst)
            if not os.path.isdir(src):
                if(self.args['buildtype'].lower() == 'debug'):
                    test_assembly_binding(src, "Microsoft.VC80.DebugCRT", "8.0.50727.4053")
                else:
                    test_assembly_binding(src, "Microsoft.VC80.CRT", "8.0.50727.4053")
                self.ccopy(src,dst)
            else:
                raise Exception("Directories are not supported by test_CRT_and_copy_action()")
        else:
            print("Doesn't exist:", src)

    def test_for_no_msvcrt_manifest_and_copy_action(self, src, dst):
        # This is used to test that no manifest for the msvcrt exists.
        # It is used as a temporary override during the construct method
        from test_win32_manifest import test_assembly_binding
        from test_win32_manifest import NoManifestException, NoMatchingAssemblyException
        # TODO: This is redundant with LLManifest.copy_action(). Why aren't we
        # calling copy_action() in conjunction with test_assembly_binding()?
        if src and (os.path.exists(src) or os.path.islink(src)):
            # ensure that destination path exists
            self.cmakedirs(os.path.dirname(dst))
            self.created_paths.append(dst)
            if not os.path.isdir(src):
                try:
                    if(self.args['buildtype'].lower() == 'debug'):
                        test_assembly_binding(src, "Microsoft.VC80.DebugCRT", "")
                    else:
                        test_assembly_binding(src, "Microsoft.VC80.CRT", "")
                    raise Exception("Unknown condition")
                except NoManifestException as err:
                    pass
                except NoMatchingAssemblyException as err:
                    pass

                self.ccopy(src,dst)
            else:
                raise Exception("Directories are not supported by test_CRT_and_copy_action()")
        else:
            print("Doesn't exist:", src)

    def construct(self):
        super().construct()

        pkgdir = os.path.join(self.args['build'], os.pardir, 'packages')
        relpkgdir = os.path.join(pkgdir, "lib", "release")
        debpkgdir = os.path.join(pkgdir, "lib", "debug")

        if self.is_packaging_viewer():
            # Find secondlife-bin.exe in the 'configuration' dir, then rename it to the result of final_exe.
            self.path(src='%s/secondlife-bin.exe' % self.args['configuration'], dst=self.final_exe())
            # <plus> SecondLifeViewer.exe left in the build folder by a build before the rename
            # (research/227 F4) is removed, so tools and the release zip find only one viewer exe.
            # Still running (locked): left, with a note; the next package step removes it.
            try:
                self.remove(self.dst_path_of("SecondLifeViewer.exe"))
            except OSError as err:
                print("Could not remove the old SecondLifeViewer.exe (still running?): %s" % err)
            # </plus>

            GITHUB_OUTPUT = os.getenv('GITHUB_OUTPUT')
            if GITHUB_OUTPUT:
                # Emit the whole app image as one of the GitHub step outputs. We
                # want the whole app -- but NOT the extraneous build products that
                # get tossed into the same directory, such as the installer and
                # the symbols tarball, so add exclusions. When we feed
                # upload-artifact multiple absolute pathnames, even just for
                # exclusion, it ends up creating several extraneous directory
                # levels within the artifact -- so try using only relative paths.
                # One problem: as of right now, our current directory os.getcwd()
                # is not the same as the initial working directory for this job
                # step, meaning paths relative to our os.getcwd() won't work for
                # the subsequent upload-artifact step. We're a couple directory
                # levels down. Try adjusting for those when specifying the base
                # for self.relpath().
                appbase = self.relpath(
                    self.get_dst_prefix(),
                    base=os.path.join(os.getcwd(), os.pardir, os.pardir),
                    symlink=True)
                self.set_github_output('viewer_app', appbase,
                                    # except for this stuff
                                    *(('!' + os.path.join(appbase, pattern))
                                        for pattern in (
                                                'secondlife-bin.*',
                                                '*_Setup.exe',
                                                '*.bat',
                                                '*.tar.xz')))

        # Plugin host application
        self.path2basename(os.path.join(os.pardir,
                                        'llplugin', 'slplugin', self.args['configuration']),
                           "slplugin.exe")

        # Get shared libs from the shared libs staging directory
        with self.prefix(src=os.path.join(self.args['build'], os.pardir,
                                          'sharedlibs', self.args['buildtype'])):
            # WebRTC libraries
            for libfile in (
                    'llwebrtc.dll',
            ):
                self.path(libfile)

            if self.args['discord'] == 'ON':
                self.path("discord_partner_sdk.dll")

            if self.args['openal'] == 'ON':
                # Get openal dll
                self.path("OpenAL32.dll")
                self.path("alut.dll")

            # For textures
            self.path("openjp2.dll")

            # These need to be installed as a SxS assembly, currently a 'private' assembly.
            # See http://msdn.microsoft.com/en-us/library/ms235291(VS.80).aspx
            self.path("msvcp140.dll")
            self.path_optional("msvcp140_1.dll")
            self.path_optional("msvcp140_2.dll")
            self.path_optional("msvcp140_atomic_wait.dll")
            self.path_optional("msvcp140_codecvt_ids.dll")
            self.path("vcruntime140.dll")
            self.path_optional("vcruntime140_1.dll")
            self.path_optional("vcruntime140_threads.dll")

            # SDL3
            self.path("SDL3.dll")

            # BugSplat
            if self.args.get('bugsplat'):
                self.path("BsSndRpt64.exe")
                self.path("BugSplat64.dll")
                self.path("BugSplatRc64.dll")

            if self.args['tracy'] == 'ON':
                with self.prefix(src=os.path.join(pkgdir, 'bin')):
                    self.path("tracy-profiler.exe")

        self.path(src="licenses-win32.txt", dst="licenses.txt")
        # <plus> Charis' own notices: what licenses.txt lacks (research/123 M2)
        self.path(src="plus/licenses-charis.txt", dst="licenses-charis.txt")
        # the license texts of all bundled packages (BSD/MIT want the full notice with binaries)
        with self.prefix(src=os.path.join(pkgdir, "LICENSES"), dst="licenses"):
            self.path("*")
        # </plus>
        self.path("featuretable.txt")
        self.path("cube.dae")

        with self.prefix(src=pkgdir):
            self.path("ca-bundle.crt")

        # Media plugins - CEF
        with self.prefix(dst="llplugin"):
            with self.prefix(src=os.path.join(self.args['build'], os.pardir, 'media_plugins')):
                with self.prefix(src=os.path.join('cef', self.args['configuration'])):
                    self.path("media_plugin_cef.dll")

                # Media plugins - LibVLC
                with self.prefix(src=os.path.join('libvlc', self.args['configuration'])):
                    self.path("media_plugin_libvlc.dll")

                # Media plugins - Example (useful for debugging - not shipped with release viewer)
                if self.channel_type() != 'release':
                    with self.prefix(src=os.path.join('example', self.args['configuration'])):
                        self.path("media_plugin_example.dll")

            # CEF runtime files - debug
            # CEF runtime files - not debug (release, relwithdebinfo etc.)
            config = 'debug' if self.args['configuration'].lower() == 'debug' else 'release'
            with self.prefix(src=os.path.join(pkgdir, 'bin', config)):
                self.path("chrome_elf.dll")
                # <plus> d3dcompiler_47.dll and dxil.dll are left out: Microsoft redistributables
                # without source (research/268). ANGLE then loads D3DCompiler_47.dll from System32
                # (part of Windows 10 and later); dxil.dll only signs DXIL for WebGPU on D3D12.
                # A copy left in the build folder by an earlier build is removed, so a run from
                # the build folder loads the same DLL as the release.
                for name in ("d3dcompiler_47.dll", "dxil.dll"):
                    self.remove(self.dst_path_of(name))
                # </plus>
                self.path("dxcompiler.dll")
                self.path("libcef.dll")
                self.path("libEGL.dll")
                self.path("libGLESv2.dll")
                self.path("v8_context_snapshot.bin")
                self.path("vk_swiftshader.dll")
                self.path("vk_swiftshader_icd.json")
                self.path("vulkan-1.dll")
                self.path("dullahan_host.exe")

            # MSVC DLLs needed for CEF and have to be in same directory as plugin
            with self.prefix(src=os.path.join(self.args['build'], os.pardir,
                                              'sharedlibs', self.args['buildtype'])):
                self.path("msvcp140.dll")
                self.path("vcruntime140.dll")
                self.path_optional("vcruntime140_1.dll")

            # CEF files common to all configurations
            with self.prefix(src=os.path.join(pkgdir, 'resources')):
                self.path("chrome_100_percent.pak")
                self.path("chrome_200_percent.pak")
                self.path("resources.pak")
                self.path("icudtl.dat")

            with self.prefix(src=os.path.join(pkgdir, 'resources', 'locales'), dst='locales'):
                self.path("am.pak")
                self.path("ar.pak")
                self.path("bg.pak")
                self.path("bn.pak")
                self.path("ca.pak")
                self.path("cs.pak")
                self.path("da.pak")
                self.path("de.pak")
                self.path("el.pak")
                self.path("en-GB.pak")
                self.path("en-US.pak")
                self.path("es-419.pak")
                self.path("es.pak")
                self.path("et.pak")
                self.path("fa.pak")
                self.path("fi.pak")
                self.path("fil.pak")
                self.path("fr.pak")
                self.path("gu.pak")
                self.path("he.pak")
                self.path("hi.pak")
                self.path("hr.pak")
                self.path("hu.pak")
                self.path("id.pak")
                self.path("it.pak")
                self.path("ja.pak")
                self.path("kn.pak")
                self.path("ko.pak")
                self.path("lt.pak")
                self.path("lv.pak")
                self.path("ml.pak")
                self.path("mr.pak")
                self.path("ms.pak")
                self.path("nb.pak")
                self.path("nl.pak")
                self.path("pl.pak")
                self.path("pt-BR.pak")
                self.path("pt-PT.pak")
                self.path("ro.pak")
                self.path("ru.pak")
                self.path("sk.pak")
                self.path("sl.pak")
                self.path("sr.pak")
                self.path("sv.pak")
                self.path("sw.pak")
                self.path("ta.pak")
                self.path("te.pak")
                self.path("th.pak")
                self.path("tr.pak")
                self.path("uk.pak")
                self.path("vi.pak")
                self.path("zh-CN.pak")
                self.path("zh-TW.pak")

            with self.prefix(src=os.path.join(pkgdir, 'bin', 'release')):
                self.path("libvlc.dll")
                self.path("libvlccore.dll")
                # <plus> VLC plugins the media plugin never uses are left out (research/268):
                # encoders (x264/x265 are GPL or commercial), stream output and muxers, DVD
                # (libdvdcss inside), Blu-ray and ARIB descrambling. Playback (access, demux,
                # decoders, audio/video output, subtitles) is unchanged. Copies left in the
                # build folder by an earlier build are removed.
                vlc_left_out = ("plugins/stream_out", "plugins/access_output", "plugins/mux",
                                "plugins/codec/libx264_plugin.dll",
                                "plugins/codec/libx26410b_plugin.dll",
                                "plugins/codec/libx265_plugin.dll",
                                "plugins/codec/libtwolame_plugin.dll",
                                "plugins/access/libdvdnav_plugin.dll",
                                "plugins/access/libdvdread_plugin.dll",
                                "plugins/access/liblibbluray_plugin.dll",
                                "plugins/access/libbluray-*.jar",
                                "plugins/stream_filter/libaribcam_plugin.dll",
                                "plugins/gui")
                for pattern in vlc_left_out:
                    for stale in glob.glob(self.dst_path_of(pattern)):
                        self.remove(stale)
                excludes = list(self.excludes)
                self.excludes += ["*/" + pattern for pattern in vlc_left_out]
                try:
                    self.path("plugins/")
                finally:
                    self.excludes = excludes
                # </plus>

        if not self.is_packaging_viewer():
            self.package_file = "copied_deps"

    def nsi_file_commands(self, install=True):
        def INSTDIR(path):
            # Note that '$INSTDIR' is purely textual here: we write
            # exactly that into the .nsi file for NSIS to interpret.
            # Pass the result through normpath() to handle the case in which
            # path is the empty string. On Windows, that produces "$INSTDIR\".
            # Unfortunately, if that's the last item on a line, NSIS takes
            # that as line continuation and misinterprets the following line.
            # Ensure we don't emit a trailing backslash.
            return os.path.normpath(os.path.join('$INSTDIR', path))

        result = []
        dest_files = [pair[1] for pair in self.file_list if pair[0] and os.path.isfile(pair[1])]
        # sort deepest hierarchy first
        dest_files.sort(key=lambda f: (f.count(os.path.sep), f), reverse=True)
        out_path = None
        for pkg_file in dest_files:
            pkg_file = os.path.normpath(pkg_file)
            rel_file = self.relpath(pkg_file)
            installed_dir = INSTDIR(os.path.dirname(rel_file))
            if install and installed_dir != out_path:
                out_path = installed_dir
                # emit SetOutPath every time it changes
                result.append('SetOutPath ' + out_path)
            if install:
                result.append('File ' + rel_file)
            else:
                result.append('Delete ' + INSTDIR(rel_file))

        # at the end of a delete, just rmdir all the directories
        if not install:
            deleted_file_dirs = [os.path.dirname(self.relpath(f)) for f in dest_files]
            # find all ancestors so that we don't skip any dirs that happened
            # to have no non-dir children
            deleted_dirs = set(itertools.chain.from_iterable(path_ancestors(d)
                                                             for d in deleted_file_dirs))
            # sort deepest hierarchy first
            for d in sorted(deleted_dirs, key=lambda f: (f.count(os.path.sep), f), reverse=True):
                result.append('RMDir ' + INSTDIR(d))

        return '\n'.join(result)

    def package_finish(self):
        # Check if we should use Velopack instead of NSIS
        # Note: as of 2026.01's release, we will be building with Velopack's one click install.
        # We maintain the legacy NSIS packaging mainly for TPVs at this point.
        if self.args.get('velopack', 'OFF') == 'ON':
            self.velopack_package_finish()
            return

        # NSIS packaging (legacy)
        self.nsis_package_finish()

    def velopack_package_finish(self):
        # <plus> Charis Viewer (research/311 sections 2.3, 2.4, 5.4). The package step does not
        # run vpk, locally or in CI: the shader bundle (app_settings/vk_shader_bundle) is made
        # after this step (research/159 section 3.3), and a Setup.exe without it must not be
        # made. So this step, the same everywhere (no GITHUB_ACTIONS branch):
        #   1. writes the file list as a build without Velopack does (secondlife_setup_tmp.nsi;
        #      NSIS is not run), so the zip, its checks and the Setup.exe start from one list;
        #   2. writes velopack-pack.json next to it: the arguments for `vpk pack`, which the
        #      release tools run once the bundle is in place (they add --packDir, the folder
        #      the zip is made from, and --outputDir).
        # The package id and the publisher are read from plus/independence/independence.h,
        # which llvelopack.cpp uses too (the install folder %LOCALAPPDATA%\<id> and the entry in
        # the installed apps list must agree). No Portable.zip (--noPortable, section 2.4).
        self.nsis_package_finish()

        pack_id = self.independence_constant('VELOPACK_PACK_ID')
        if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]*', pack_id):
            raise ManifestError("VELOPACK_PACK_ID %r is not a Velopack package id" % pack_id)
        authors = self.independence_constant('PUBLISHER')
        # Velopack requires SemVer2: Charis's version and build number, ordered by SemVer
        # (0.1.0-alpha.1.<build> < 0.1.0-alpha.2.<build> < 0.1.0), not the official version
        # (plus/version/packversion.py). It is also the DisplayVersion of the installed apps
        # list (llvelopack.cpp register_uninstall_info() takes Velopack's app_version).
        pack_version, version_info = self.charis_version()
        # The title Setup.exe shows ("Charis Viewer"): version.h's PRODUCT_NAME, as the
        # installed apps list (llvelopack.cpp's own entry). vpk also names the launcher stub
        # in the install folder after it ("Charis Viewer.exe"); llvelopack.cpp's shortcuts
        # and its entry name current\CharisViewer.exe, never the stub.
        pack_title = version_info['product_name']
        main_exe = self.final_exe()
        # --packDir must be the folder the zip is made from (the files of the .nsi list and the
        # shader bundle), never the build folder: it holds more than these patterns leave out
        # (logs, stamps, leftovers). The patterns are only a second line of defence.
        exclude_pattern = r'.*\.pdb|.*\.map|.*\.bat|.*\.exp|.*\.lib|.*\.nsi|.*\.tar\.xz|secondlife-bin\..*|.*_Setup\.exe|.*-Setup\.exe|velopack-pack\.json'

        # Channel-specific icon for the Velopack installer.
        # CMake copies icons/{channel}/secondlife.ico to res/ll_icon.ico at configure time.
        # Try the CMake-generated copy first, fall back to the source icon.
        icon_path = os.path.join(self.get_src_prefix(), 'res', 'll_icon.ico')
        if not os.path.exists(icon_path):
            icon_path = os.path.join(self.get_src_prefix(), self.icon_path(), 'secondlife.ico')
        if not os.path.exists(icon_path):
            raise ManifestError("no icon for Setup.exe at %s" % icon_path)

        vpk_args = [
            'pack',
            '--packId', pack_id,
            '--packVersion', pack_version,
            '--packTitle', pack_title,
            '--packAuthors', authors,
            '--mainExe', main_exe,
            '--exclude', exclude_pattern,
            # Suppress Velopack's built-in shortcut creation; we create our own
            # shortcuts in llvelopack.cpp on_after_install hook instead.
            '--shortcuts', '',
            '--noPortable',
            # The viewer is 64-bit: without the architecture vpk packs its x86 Update.exe
            # and stub ("No architecture specified with --runtime, defaulting to x86").
            # plus/building/release/releasecommon.py VPK_RUNTIME checks it: change both.
            '--runtime', 'win-x64',
            '--icon', os.path.abspath(icon_path),
        ]
        pack = {
            'what': 'Arguments for vpk pack (Charis Viewer, research/311). Add --outputDir, and --packDir: '
                    'the folder the zip is made from (never the build folder).',
            'packId': pack_id,
            'packVersion': pack_version,
            'mainExe': main_exe,
            'buildDir': os.path.abspath(self.get_dst_prefix()),
            'vpkArgs': vpk_args,
        }
        pack_json = os.path.join(self.get_dst_prefix(), 'velopack-pack.json')
        with open(pack_json, 'w', encoding='utf-8', newline='\n') as f:
            json.dump(pack, f, indent=2)
            f.write('\n')
        print("Velopack: vpk pack is run after the shader bundle; its arguments are in %s" % pack_json)
        self.set_github_output('velopack_pack_json', os.path.abspath(pack_json))

    def independence_constant(self, name):
        """The "..." value of `inline constexpr char <name>[] = "...";` in
        plus/independence/independence.h, so the package and the viewer share it."""
        path = os.path.join(self.get_src_prefix(), 'plus', 'independence', 'independence.h')
        with open(path, encoding='utf-8') as f:
            text = f.read()
        found = re.findall(r'^[ \t]*inline constexpr char %s\[\] = "([^"\\\n]+)";' % re.escape(name), text, re.M)
        if len(found) != 1:
            raise ManifestError("%s: expected one plain constant %s, found %d" % (path, name, len(found)))
        return found[0]
        # </plus>

    def nsis_package_finish(self):
        """Package the viewer using NSIS installer (legacy)"""
        # a standard map of strings for replacing in the templates
        substitution_strings = {
            'version' : '.'.join(self.args['version']),
            'version_short' : '.'.join(self.args['version'][:-1]),
            'version_dashes' : '-'.join(self.args['version']),
            'version_registry' : '%s(64)' % '.'.join(self.args['version']),
            'final_exe' : self.final_exe(),
            'flags':'',
            'app_name':self.app_name(),
            'app_name_oneword':self.app_name_oneword()
            }

        installer_file = self.installer_base_name() + '_Setup.exe'
        substitution_strings['installer_file'] = installer_file

        version_vars = """
        !define INSTEXE "%(final_exe)s"
        !define VERSION "%(version_short)s"
        !define VERSION_LONG "%(version)s"
        !define VERSION_DASHES "%(version_dashes)s"
        !define VERSION_REGISTRY "%(version_registry)s"
        !define VIEWER_EXE "%(final_exe)s"
        """ % substitution_strings

        if self.channel_type() == 'release':
            substitution_strings['caption'] = CHANNEL_VENDOR_BASE
        else:
            substitution_strings['caption'] = self.app_name() + ' ${VERSION}'

        inst_vars_template = """
            OutFile "%(installer_file)s"
            !define INSTNAME   "%(app_name_oneword)s"
            !define SHORTCUT   "%(app_name)s"
            !define URLNAME   "secondlife"
            Caption "%(caption)s"
            """

        engage_registry="SetRegView 64"
        program_files="!define MULTIUSER_USE_PROGRAMFILES64"

        # Dump the installers/windows directory into the raw app image tree
        # because NSIS needs those files. But don't use path() because we
        # don't want them installed with the viewer - they're only for use by
        # the installer itself.
        shutil.copytree(os.path.join(self.get_src_prefix(), 'installers', 'windows'),
                        os.path.join(self.get_dst_prefix(), 'installers', 'windows'),
                        dirs_exist_ok=True)

        tempfile = "secondlife_setup_tmp.nsi"
        # the following replaces strings in the nsi template
        # it also does python-style % substitution
        self.replace_in("installers/windows/installer_template.nsi", tempfile, {
                "%%VERSION%%":version_vars,
                # The template references "%%SOURCE%%\installers\windows\...".
                # Now that we've copied that directory into the app image
                # tree, we can just replace %%SOURCE%% with '.'.
                "%%SOURCE%%":'.',
                "%%INST_VARS%%":inst_vars_template % substitution_strings,
                "%%INSTALL_FILES%%":self.nsi_file_commands(True),
                "%%PROGRAMFILES%%":program_files,
                "%%ENGAGEREGISTRY%%":engage_registry,
                "%%DELETE_FILES%%":self.nsi_file_commands(False)})

        self.package_file = installer_file


class Darwin_x86_64_Manifest(ViewerManifest):
    build_data_json_platform = 'mac'
    address_size = 64

    def finish_build_data_dict(self, build_data_dict):
        build_data_dict.update({'Bundle Id':self.args['bundleid']})
        return build_data_dict

    def is_packaging_viewer(self):
        # darwin requires full app bundle packaging even for debugging.
        return True

    def is_rearranging(self):
        # That said, some stuff should still only be performed once.
        # Are either of these actions in 'actions'? Is the set intersection
        # non-empty?
        return bool(set(["package", "unpacked"]).intersection(self.args['actions']))

    def construct(self):
        # copy over the build result (this is a no-op if run within the xcode
        # script)
        self.path(os.path.join(self.args['configuration'], self.channel() + ".app"), dst="")

        pkgdir = os.path.join(self.args['build'], os.pardir, 'packages')
        relpkgdir = os.path.join(pkgdir, "lib", "release")
        debpkgdir = os.path.join(pkgdir, "lib", "debug")

        with self.prefix(src="", dst="Contents"):  # everything goes in Contents
            bugsplat_db = self.args.get('bugsplat')
            if bugsplat_db:
                # Inject Bugsplat's db into Info.plist if provided.
                Info_plist = self.dst_path_of("Info.plist")
                with open(Info_plist, 'rb') as f:
                    Info = plistlib.load(f)
                    # https://www.bugsplat.com/docs/platforms/os-x#configuration
                    Info["BugSplatDatabase"] = bugsplat_db
                    self.put_in_file(
                        plistlib.dumps(Info),
                        os.path.basename(Info_plist),
                        "Info.plist")

            # CEF framework goes inside Contents/Frameworks.
            # Remember where we parked this car.
            with self.prefix(src=relpkgdir, dst="Frameworks"):
                self.path("libndofdev.dylib")
                self.path("libSDL3.dylib") # gamecontrol

                if self.args.get('bugsplat'):
                    self.path2basename(relpkgdir, "BugsplatMac.framework")
                    self.path2basename(relpkgdir, "CrashReporter.framework")
                    self.path2basename(relpkgdir, "HockeySDK.framework")

                # OpenAL dylibs
                if self.args['openal'] == 'ON':
                    for libfile in (
                                "libopenal.dylib",
                                "libalut.dylib",
                                ):
                        self.path(libfile)

                # WebRTC libraries
                with self.prefix(src=os.path.join(self.args['build'], os.pardir,
                                          'sharedlibs', self.args['buildtype'], 'Resources')):
                    for libfile in (
                            'libllwebrtc.dylib',
                    ):
                        self.path(libfile)

            with self.prefix(dst="MacOS"):
                executable = self.dst_path_of(self.channel())
                if self.args.get('bugsplat'):
                    # According to Apple Technical Note TN2206:
                    # https://developer.apple.com/library/archive/technotes/tn2206/_index.html#//apple_ref/doc/uid/DTS40007919-CH1-TNTAG207
                    # "If an app uses @rpath or an absolute path to link to a
                    # dynamic library outside of the app, the app will be
                    # rejected by Gatekeeper. ... Neither the codesign nor the
                    # spctl tool will show the error."
                    # (Thanks, Apple. Maybe fix spctl to warn?)
                    # The BugsplatMac framework embeds @rpath, which is
                    # causing scary Gatekeeper popups at viewer start. Work
                    # around this by changing the reference baked into our
                    # viewer. The install_name_tool -change option needs the
                    # previous value. Instead of guessing -- which might
                    # silently be defeated by a BugSplat SDK update that
                    # changes their baked-in @rpath -- ask for the path
                    # stamped into the framework.
                    # Let exception, if any, propagate -- if this doesn't
                    # work, we need the build to noisily fail!
                    oldpath = subprocess.check_output(
                        ['objdump', '--macho', '--dylib-id', '--non-verbose',
                         os.path.join(relpkgdir, "HockeySDK.framework", "HockeySDK")],
                        text=True
                        ).splitlines()[-1]  # take the last line of output
                    self.run_command(
                        ['install_name_tool', '-change', oldpath,
                         '@executable_path/../Frameworks/HockeySDK.framework/HockeySDK',
                         executable])
                    oldpath = subprocess.check_output(
                        ['objdump', '--macho', '--dylib-id', '--non-verbose',
                         os.path.join(relpkgdir, "CrashReporter.framework", "CrashReporter")],
                        text=True
                        ).splitlines()[-1]  # take the last line of output
                    self.run_command(
                        ['install_name_tool', '-change', oldpath,
                         '@executable_path/../Frameworks/CrashReporter.framework/CrashReporter',
                         executable])
                    oldpath = subprocess.check_output(
                        ['objdump', '--macho', '--dylib-id', '--non-verbose',
                         os.path.join(relpkgdir, "BugsplatMac.framework", "BugsplatMac")],
                        text=True
                        ).splitlines()[-1]  # take the last line of output
                    self.run_command(
                        ['install_name_tool', '-change', oldpath,
                         '@executable_path/../Frameworks/BugsplatMac.framework/BugsplatMac',
                         executable])

                # NOTE: the -S argument to strip causes it to keep
                # enough info for annotated backtraces (i.e. function
                # names in the crash log). 'strip' with no arguments
                # yields a slightly smaller binary but makes crash
                # logs mostly useless. This may be desirable for the
                # final release. Or not.
                if ("package" in self.args['actions'] or
                    "unpacked" in self.args['actions']):
                    self.run_command(
                        ['strip', '-S', executable])

            with self.prefix(dst="Resources"):
                # defer cross-platform file copies until we're in the
                # nested Resources directory
                super().construct()

                # need .icns file referenced by Info.plist
                with self.prefix(src=self.icon_path(), dst="") :
                    self.path("secondlife.icns")

                with self.prefix(src_dst="cursors_mac"):
                    self.path("*.tif")

                self.path("licenses-mac.txt", dst="licenses.txt")
                self.path("featuretable_mac.txt")
                self.path("cube.dae")

                with self.prefix(src=pkgdir,dst=""):
                    self.path("ca-bundle.crt")

                # Translations
                self.path("English.lproj/language.txt")
                self.replace_in(src="English.lproj/InfoPlist.strings",
                                dst="English.lproj/InfoPlist.strings",
                                searchdict={'%%VERSION%%':'.'.join(self.args['version'])}
                                )
                self.path("German.lproj")
                self.path("Japanese.lproj")
                self.path("Korean.lproj")
                self.path("da.lproj")
                self.path("es.lproj")
                self.path("fr.lproj")
                self.path("hu.lproj")
                self.path("it.lproj")
                self.path("nl.lproj")
                self.path("pl.lproj")
                self.path("pt.lproj")
                self.path("ru.lproj")
                self.path("tr.lproj")
                self.path("uk.lproj")
                self.path("zh-Hans.lproj")

                def path_optional(src, dst):
                    """
                    For a number of our self.path() calls, not only do we want
                    to deal with the absence of src, we also want to remember
                    which were present. Return either an empty list (absent)
                    or a list containing dst (present). Concatenate these
                    return values to get a list of all libs that are present.
                    """
                    # This was simple before we started needing to pass
                    # wildcards. Fortunately, self.path() ends up appending a
                    # (source, dest) pair to self.file_list for every expanded
                    # file processed. Remember its size before the call.
                    oldlen = len(self.file_list)
                    try:
                        self.path(src, dst)
                        # The dest appended to self.file_list has been prepended
                        # with self.get_dst_prefix(). Strip it off again.
                        added = [os.path.relpath(d, self.get_dst_prefix())
                                 for s, d in self.file_list[oldlen:]]
                    except MissingError as err:
                        print("Warning: "+err.msg, file=sys.stderr)
                        added = []
                    if not added:
                        print("Skipping %s" % dst)
                    return added

                # dylibs is a list of all the .dylib files we expect to need
                # in our bundled sub-apps. For each of these we'll create a
                # symlink from sub-app/Contents/Resources to the real .dylib.
                # Need to get the llcommon dll from any of the build directories as well.
                libfile_parent = self.get_dst_prefix()
                dylibs=[]

                # Discord social SDK
                if self.args['discord'] == 'ON':
                    for libfile in (
                                "libdiscord_partner_sdk.dylib",
                                ):
                        self.path2basename(relpkgdir, libfile)

                # our apps
                executable_path = {}
                embedded_apps = [ (os.path.join("llplugin", "slplugin"), "SLPlugin.app") ]
                for app_bld_dir, app in embedded_apps:
                    self.path2basename(os.path.join(os.pardir,
                                                    app_bld_dir, self.args['configuration']),
                                       app)
                    executable_path[app] = \
                        self.dst_path_of(os.path.join(app, "Contents", "MacOS"))

                # Dullahan helper apps go inside SLPlugin.app
                with self.prefix(dst=os.path.join(
                    "SLPlugin.app", "Contents", "Frameworks")):
                    # copy CEF plugin
                    self.path2basename("../media_plugins/cef/" + self.args['configuration'],
                                       "media_plugin_cef.dylib")

                    # copy LibVLC plugin
                    self.path2basename("../media_plugins/libvlc/" + self.args['configuration'],
                                       "media_plugin_libvlc.dylib")

                    # CEF framework and vlc libraries goes inside Contents/Frameworks.
                    with self.prefix(src=os.path.join(pkgdir, 'lib', 'release')):
                        self.path("Chromium Embedded Framework.framework")
                        self.path("DullahanHelper.app")
                        self.path("DullahanHelper (Alerts).app")
                        self.path("DullahanHelper (GPU).app")
                        self.path("DullahanHelper (Renderer).app")
                        self.path("DullahanHelper (Plugin).app")

                        # Copy libvlc
                        self.path( "libvlc*.dylib*" )
                        # copy LibVLC plugins folder
                        with self.prefix(src='plugins', dst="plugins"):
                            self.path( "*.dylib" )
                            self.path( "plugins.dat" )


    def package_finish(self):
        imagename = self.installer_base_name_mac()
        self.set_github_output('imagename', imagename)
        finalname = imagename + ".dmg"
        self.package_file = finalname

        RUNNER_TEMP = os.getenv('RUNNER_TEMP')
        # When running as a GitHub Action job, RUNNER_TEMP is the recommended
        # temp directory. If we're not running on GitHub, don't create this
        # temp directory or this tarball: we don't clean them up, trusting
        # that the runner is itself transient. On a dev machine, that would
        # result in temp-directory clutter.
        if RUNNER_TEMP:
            # Per GitHub's actions/upload-artifact documentation
            # https://github.com/actions/upload-artifact#maintaining-file-permissions-and-case-sensitive-files
            # we must package the app bundle with tar before posting as an
            # artifact. Posting individual files follows symlinks, which
            # causes problems, especially with frameworks: a framework's top
            # level must contain symlinks into its Versions/Current, which
            # must itself be a symlink to some specific Versions subdir.
            tarpath = os.path.join(RUNNER_TEMP, "viewer.tar.xz")
            print(f'Creating {tarpath} from {self.get_dst_prefix()}')
            with tarfile.open(tarpath, mode="w:xz") as tarball:
                # Store in the tarball as just 'Second Life Mumble.app'
                # instead of 'Users/someone/.../newview/Release/Second...'
                # It's at this point that we rename 'Second Life Release.app'
                # to 'Second Life Viewer.app'.
                tarball.add(self.get_dst_prefix(),
                            arcname=self.app_name() + ".app")
            self.set_github_output_path('viewer_app', tarpath)

        # Generate Velopack update packages if enabled
        # This creates the nupkg and RELEASES files needed for auto-updates
        # Distribution is still via DMG, but updates use Velopack
        if self.args.get('velopack', 'OFF') == 'ON':
            self.velopack_package_finish()

    def velopack_package_finish(self):
        """Generate Velopack update packages for macOS.

        This creates the nupkg and releases.json files needed for auto-updates.
        Distribution is still via DMG - Velopack only handles the update infrastructure.
        """
        # packId determines install identification - same as Windows for consistency
        pack_id = self.app_name_oneword()  # "SecondLife", "SecondLifeBeta", etc.
        # <plus> Velopack requires SemVer2: Charis's version, as on Windows (charis_version()).
        # The rest of this macOS step is still the official one (no Charis macOS release yet).
        pack_version, _ = self.charis_version()
        # </plus>
        pack_title = self.app_name()  # Display name with spaces

        # The .app bundle path (e.g., "/path/to/Second Life Release.app")
        app_bundle = self.get_dst_prefix()
        # Bundle ID from args (e.g., "com.secondlife.viewer")
        bundle_id = self.args.get('bundleid', 'com.secondlife.indra.viewer')

        # Icon path for macOS
        icon_path = os.path.join(self.get_src_prefix(), self.icon_path(), 'secondlife.icns')

        # The main executable inside Contents/MacOS/ is named after the channel
        main_exe = self.channel()

        # In CI, defer Velopack packaging to the sign step where code signing
        # credentials are available. Emit metadata as GitHub outputs so the
        # sign step can run vpk pack after signing the app bundle.
        if os.getenv('GITHUB_ACTIONS'):
            self.set_github_output('velopack_mac_pack_id', pack_id)
            self.set_github_output('velopack_mac_pack_version', pack_version)
            self.set_github_output('velopack_mac_pack_title', pack_title)
            self.set_github_output('velopack_mac_main_exe', main_exe)
            self.set_github_output('velopack_mac_bundle_id', bundle_id)
            print("CI mode: macOS Velopack packaging deferred to sign step")
            return

        # Local builds: run vpk pack directly (unsigned)

        # Parent directory containing the .app bundle - this is where we run vpk from
        # and where the Releases directory will be created
        work_dir = os.path.dirname(app_bundle)

        # Output directory for releases - clean it first to avoid version conflicts
        releases_dir = os.path.join(work_dir, 'Releases')
        if os.path.exists(releases_dir):
            print("Cleaning existing Releases directory: %s" % releases_dir)
            shutil.rmtree(releases_dir)

        # Build vpk command for macOS
        # See: https://docs.velopack.io/reference/cli/content/vpk-osx
        vpk_args = [
            'vpk', 'pack',
            '--packId', pack_id,
            '--packVersion', pack_version,
            '--packDir', app_bundle,
            '--packTitle', pack_title,
            '--mainExe', main_exe,  # Executable name inside Contents/MacOS/
            '--bundleId', bundle_id,
            '--outputDir', releases_dir,
            '--noInst',  # Don't generate .pkg installer - we use DMG for distribution
            '--verbose',  # Show detailed output
        ]

        # Add icon if exists
        if os.path.exists(icon_path):
            vpk_args.extend(['--icon', icon_path])

        print("Running Velopack packaging for macOS:")
        print("  Command: %s" % ' '.join(vpk_args))
        print("  Working directory: %s" % work_dir)
        print("  App bundle: %s" % app_bundle)
        print("  Main executable: %s" % main_exe)

        # Run vpk command
        result = subprocess.run(vpk_args, cwd=work_dir, capture_output=True, text=True)

        # Always print output for debugging
        if result.stdout:
            print("vpk stdout:\n%s" % result.stdout)
        if result.stderr:
            print("vpk stderr:\n%s" % result.stderr)

        if result.returncode != 0:
            raise ManifestError("Velopack packaging failed with code %d" % result.returncode)

        # Verify the Releases directory was created and contains expected files
        if not os.path.exists(releases_dir):
            raise ManifestError("Velopack releases directory not found: %s" % releases_dir)

        # List what was created
        releases_contents = os.listdir(releases_dir)
        print("Velopack releases directory contents: %s" % releases_contents)

        # Verify we have the expected files (nupkg and releases JSON)
        nupkg_files = [f for f in releases_contents if f.endswith('.nupkg')]
        json_files = [f for f in releases_contents if f.endswith('.json')]

        if not nupkg_files:
            raise ManifestError("No .nupkg files found in releases directory")
        if not json_files:
            raise ManifestError("No releases JSON files found in releases directory")

        print("Generated %d nupkg file(s): %s" % (len(nupkg_files), nupkg_files))
        print("Generated %d JSON file(s): %s" % (len(json_files), json_files))

        # Output the Releases directory path for artifact upload
        self.set_github_output('velopack_releases', releases_dir)
        print("Velopack releases directory: %s" % releases_dir)


class LinuxManifest(ViewerManifest):
    build_data_json_platform = 'lnx'

    def construct(self):
        super(LinuxManifest, self).construct()

        pkgdir = os.path.join(self.args['build'], os.pardir, 'packages')
        if "package_dir" in self.args:
            pkgdir = self.args['package_dir']

        relpkgdir = os.path.join(pkgdir, "lib", "release")
        debpkgdir = os.path.join(pkgdir, "lib", "debug")

        self.path("licenses-linux.txt","licenses.txt")
        with self.prefix("linux_tools"):
            self.path("wrapper.sh","secondlife")
            with self.prefix(dst="etc"):
                self.path("handle_secondlifeprotocol.sh")
                self.path("register_secondlifeprotocol.sh")
                self.path("refresh_desktop_app_entry.sh")
            self.path("install.sh")

        with self.prefix(dst="bin"):
            self.path("secondlife-bin","do-not-directly-run-secondlife-bin")
            self.path2basename("../llplugin/slplugin", "SLPlugin")
            #this copies over the python wrapper script, associated utilities and required libraries, see SL-321, SL-322 and SL-323
            #with self.prefix(src="../viewer_components/manager", dst=""):
            #    self.path("*.py")

        # recurses, packaged again
        self.path("res-sdl")

        # We  copy ll_icon.BMP in CMakeLists.txt to newview/res-sdl and this will let the above self.path step take  care of copying
        # the correct branded icon
        # Get the icons based on the channel type
        icon_path = self.icon_path()
        print("DEBUG: icon_path '%s'" % icon_path)
        with self.prefix(src=icon_path) :
            self.path("secondlife_256.png","secondlife_icon.png")
            with self.prefix(dst="res-sdl") :
                self.path("secondlife_256.BMP","ll_icon.BMP")

        with self.prefix(src=os.path.join(self.args['build'], os.pardir, "llwebrtc" ), dst="lib"):
            self.path("libllwebrtc.so")

        # plugins
        with self.prefix(dst="bin/llplugin"):
            with self.prefix(src=os.path.join(self.args['build'], os.pardir, 'media_plugins')):
                with self.prefix(src='cef'):
                    self.path("libmedia_plugin_cef.so")

                # Media plugins - LibVLC
                with self.prefix(src='libvlc'):
                    self.path("libmedia_plugin_libvlc.so")

                # GStreamer 1.0 Media Plugin
                with self.prefix(src='gstreamer10'):
                    self.path("libmedia_plugin_gstreamer10.so")

                # Media plugins - Example (useful for debugging - not shipped with release viewer)
                if self.channel_type() != 'release':
                    with self.prefix(src='example'):
                        self.path("libmedia_plugin_example.so")


        with self.prefix(src=os.path.join(pkgdir, 'lib', 'release'), dst="lib"):
            self.path( "libcef.so" )
            self.path( "libEGL*" )
            self.path( "libvulkan*" )
            self.path( "libvk_swiftshader*" )
            self.path( "libGLESv2*" )

        with self.prefix(src=os.path.join(pkgdir, 'bin', 'release'), dst="bin"):
            self.path( "chrome-sandbox" )
            self.path( "dullahan_host" )

        with self.prefix(src=os.path.join(pkgdir, 'lib', 'release'), dst="bin"):
            self.path( "v8_context_snapshot.bin" )
            self.path( "vk_swiftshader_icd.json")

        with self.prefix(src=os.path.join(pkgdir, 'lib', 'release'), dst="lib"):
            self.path( "v8_context_snapshot.bin" )
            self.path( "vk_swiftshader_icd.json")

        with self.prefix(src=os.path.join(pkgdir, 'resources'), dst="lib"):
            self.path( "chrome_100_percent.pak" )
            self.path( "chrome_200_percent.pak" )
            self.path( "resources.pak" )
            self.path( "icudtl.dat" )

        with self.prefix(src=os.path.join(pkgdir, 'resources', 'locales'), dst=os.path.join('lib', 'locales')):
            self.path("*.pak")

        self.path("featuretable_linux.txt")
        self.path("cube.dae")

        with self.prefix(src=pkgdir, dst="bin"):
            self.path("ca-bundle.crt")

    def package_finish(self):
        installer_name = self.installer_base_name()

        # When running as a GitHub Action job, RUNNER_TEMP is defined as the tmp dir
        RUNNER_TEMP = os.getenv('RUNNER_TEMP')

        self.strip_binaries()

        # Fix access permissions
        self.run_command(['find', self.get_dst_prefix(),
                          '-type', 'd', '-exec', 'chmod', '755', '{}', ';'])
        for old, new in ('0700', '0755'), ('0500', '0555'), ('0600', '0644'), ('0400', '0444'):
            self.run_command(['find', self.get_dst_prefix(),
                              '-type', 'f', '-perm', old,
                              '-exec', 'chmod', new, '{}', ';'])
        self.package_file = installer_name + '.tar.xz'

        # temporarily move directory tree so that it has the right
        # name in the tarfile
        realname = self.get_dst_prefix()
        versionedName = self.build_path_of(installer_name)

        tarName = versionedName + ".tar.xz"

        # If using a github runner we divert packaging a little. Considering this wil be a VM/docker image
        # we can just pack the final installer into RUNNER_TEMP and not into the usual stop we'd pick when
        # not building a GHA release
        if RUNNER_TEMP:
            tarName = os.path.join(RUNNER_TEMP, self.package_file)

        self.run_command(["mv", realname, versionedName])

        try:
            # only create tarball if it's a release build.
            if self.args['buildtype'].lower() == 'release':
                # --numeric-owner hides the username of the builder for
                # security etc.
                self.run_command(['tar', '-C', self.get_build_prefix(),
                                  '--numeric-owner', '-cJf',
                                 tarName, installer_name])
                self.set_github_output_path('viewer_app', tarName)
            else:
                print("Skipping %s.tar.xz for non-Release build (%s)" % \
                      (installer_name, self.args['buildtype']))
        finally:
            self.run_command(["mv", versionedName, realname])

    def strip_binaries(self):
        if self.args['buildtype'].lower() == 'release' and self.is_packaging_viewer():
            print("* Going strip-crazy on the packaged binaries, since this is a Release build")
            # makes some small assumptions about our packaged dir structure
            self.run_command(
                ["find"] +
                [os.path.join(self.get_dst_prefix(), dir) for dir in ('bin', 'lib')] +
                ['-type', 'f', '!', '-name', '*.py',
                 '!', '-name', '*.pak',
                 '!', '-name', '*.bin',
                 '!', '-name', '*.dat',
                 '!', '-name', '*.crt',
                 '!', '-name', '*.dll',
                 '!', '-name', '*.lib',
                 '!', '-name', '*.json',
                 '!', '-name', 'update_install', '-exec', 'strip', '-S', '{}', ';'])

class Linux_x86_64_Manifest(LinuxManifest):
    address_size = 64

    def construct(self):
        super(Linux_x86_64_Manifest, self).construct()

        pkgdir = os.path.join(self.args['build'], os.pardir, 'packages')
        if "package_dir" in self.args:
            pkgdir = self.args['package_dir']

        relpkgdir = os.path.join(pkgdir, "lib", "release")
        #debpkgdir = os.path.join(pkgdir, "lib", "debug")

        with self.prefix(src=relpkgdir, dst="lib"):
            self.path("libSDL*.so.*")

            self.path("libalut.so*")
            self.path("libopenal.so*")

            if self.args['discord'] == 'ON':
                self.path("libdiscord_partner_sdk.so*")

        self.strip_binaries()
################################################################

if __name__ == "__main__":
    # Report our own command line so that, in case of trouble, a developer can
    # manually rerun the same command.
    print(('%s \\\n%s' %
          (sys.executable,
           ' '.join((("'%s'" % arg) if ' ' in arg else arg) for arg in sys.argv))))
    extra_arguments = [
        dict(name='bugsplat', description="""BugSplat database to which to post crashes,
             if BugSplat crash reporting is desired""", default=''),
        dict(name='discord', description="""Indication discord social sdk libraries are needed""", default='OFF'),
        dict(name='openal', description="""Indication openal libraries are needed""", default='OFF'),
        dict(name='tracy', description="""Indication tracy profiler is enabled""", default='OFF'),
        dict(name='velopack', description="""Use Velopack installer instead of NSIS""", default='OFF'),
        ]
    try:
        main(extra=extra_arguments)
    except (ManifestError, MissingError) as err:
        sys.exit("\nviewer_manifest.py failed: "+err.msg)
    except:
        raise
