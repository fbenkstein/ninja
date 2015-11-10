#!/usr/bin/env python
try:
    import setuptools
except ImportError:
    pass

from distutils.core import setup, Extension
from distutils.command.build import build
from distutils.command.build_ext import build_ext
from distutils.command.build_py import build_py
from distutils import log

import os
import platform
import re
import errno
import glob

cmdclass = {}

# By default distutils' swig support generates output files into the
# source directory.  This subclass generates it into a temporary
# directory instead.  Also take care that the generated Python module
# gets installed correctly.
class SwigBuildExt(build_ext):
    user_options = build_ext.user_options + [
        ('build-swig=', None, "directory for generated swig files"),
    ]

    def initialize_options(self):
        build_ext.initialize_options(self)
        self.build_base = None
        self.build_swig = None

    def finalize_options(self):
        build_ext.finalize_options(self)
        self.set_undefined_options("build", ("build_base", "build_base"))

        if self.build_swig is None:
            self.build_swig = os.path.join(self.build_base, "swig")

    def swig_sources(self, sources, extension):
        swig_sources = []
        swig_targets = {}
        new_sources = []

        target_ext = ".cpp" if "-c++" in (self.swig_opts + extension.swig_opts) else ".c"

        for source in sources:
            base, ext = os.path.splitext(source)

            if ext == ".i":
                new_sources.append(os.path.join(self.build_swig, base) + "_wrap" + target_ext)
                swig_sources.append(source)
                swig_targets[source] = new_sources[-1]
            else:
                new_sources.append(source)

        if not swig_sources:
            return new_sources

        swig = self.find_swig()
        swig_cmd = [swig, "-python"]
        swig_cmd.extend(self.swig_opts)

        if not self.swig_opts:
            for o in extension.swig_opts:
                swig_cmd.append(o)

        extdir = os.path.dirname(self.get_ext_fullpath(extension.name))
        self.mkpath(extdir)

        swig_cmd.extend(("-outdir", extdir))

        for source in swig_sources:
            target = swig_targets[source]
            target_header = os.path.splitext(target)[0] + ".h"
            log.info("swigging %s to %s", source, target)
            self.mkpath(os.path.dirname(target))
            self.spawn(swig_cmd + ["-o", target, "-oh", target_header, source])

        return new_sources

cmdclass["build_ext"] = SwigBuildExt

_pyninja_sources = [
    'src/pyninja.i',
    'src/build.cc',
    'src/build_log.cc',
    'src/clean.cc',
    'src/debug_flags.cc',
    'src/depfile_parser.cc',
    'src/deps_log.cc',
    'src/hash_log.cc',
    'src/disk_interface.cc',
    'src/edit_distance.cc',
    'src/eval_env.cc',
    'src/graph.cc',
    'src/graphviz.cc',
    'src/lexer.cc',
    'src/line_printer.cc',
    'src/manifest_parser.cc',
    'src/metrics.cc',
    'src/state.cc',
    'src/util.cc',
    'src/version.cc',
]

if platform.system() == "Windows":
    for name in ['subprocess-win32',
                 'includes_normalize-win32',
                 'msvc_helper-win32',
                 'msvc_helper_main-win32']:
        _pyninja_sources.append('src/%s.cc' % name)
    if platform.python_compiler().startswith("MSC"):
        _pyninja_sources.append('src/minidump-win32.cc')
    _pyninja_sources.append('src/getopt.cc')
else:
    _pyninja_sources.append('src/subprocess-posix.cc')

if platform.python_compiler().startswith("MSC"):
    extra_compile_args = [
        '/W4',  # Highest warning level.
        '/WX',  # Warnings as errors.
        '/wd4530', '/wd4100', '/wd4706',
        '/wd4512', '/wd4800', '/wd4702', '/wd4819',
        # Disable warnings about passing "this" during initialization.
        '/wd4355',
        '/wd4267',
    ]
    define_macros = [
        ('NOMINMAX', None),
        ('_CRT_SECURE_NO_WARNINGS', None),
        ('_VARIADIC_MAX', '10'),
    ]
else:
    extra_compile_args = [
        '-Wno-deprecated',
        '-Wno-missing-field-initializers',
        '-Wno-unused-parameter',
    ]
    define_macros = []

if platform.system() == "Linux":
    define_macros.append(("USE_POLL", None))

# extract version number from sources
with open(os.path.join("src", "version.cc")) as f:
    content = f.read()
    r = re.compile(r'const char\* kNinjaVersion = "(.*)";')
    version = r.search(content).group(1)

_pyninja_module = Extension(
    "_pyninja",
    _pyninja_sources,
    swig_opts=[
        "-I-",
        "-Isrc",
        "-c++",
        "-Wextra",
        "-threads",
        "-builtin",
        "-modern",
        "-modernargs",
        "-extranative",
    ],
    extra_compile_args=extra_compile_args,
    include_dirs=["src"],
    define_macros=define_macros,
    depends=["src/pyninja.in.py"],
    language="c++",
)

setup(
    name="pyninja",
    ext_modules=[_pyninja_module],
    version=version,
    cmdclass=cmdclass,
)
