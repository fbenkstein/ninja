# This module is not complete.  It is verbatim into the actual module
# created by SWIG and is kept as a separate file only because it
# allows easier editing.

# In the SWIG module this future statement will be at the top.  It
# can't be repeated here because this would lead to a SyntaxError.
# from __future__ import print_function, division

import sys
import os
import errno
import multiprocessing

class NinjaMain(BuildLogUser):
    def __init__(self, config):
        super(NinjaMain, self).__init__()
        self.config = config
        self.state = State()
        self.build_dir = None
        self.disk_interface = RealDiskInterface()
        self.build_log = BuildLog()
        self.deps_log = DepsLog()

    def EnsureBuildDirExists(self):
        self.build_dir = self.state.bindings_.LookupVariable("builddir")

        if self.build_dir and not self.config.dry_run:
            try:
                os.mkdir(self.build_dir)
            except OSError as e:
                if e.errno != errno.EEXIST:
                    raise NinjaError(str(e))

    def OpenBuildLog(self):
        log_path = ".ninja_log"

        if self.build_dir:
            log_path = os.path.join(self.build_dir, log_path)

        try:
            self.build_log.Load(log_path)
        except NinjaError as e:
            raise NinjaError("loading build log %s: %s" % log_path, e)

        if not self.config.dry_run:
            try:
                self.build_log.OpenForWrite(log_path, self)
            except NinjaError as e:
                raise NinjaError("opening build log: %s" % e)

    def OpenDepsLog(self):
        log_path = ".ninja_deps"

        if self.build_dir:
            log_path = os.path.join(self.build_dir, log_path)

        try:
            self.deps_log.Load(log_path, self.state)
        except NinjaError as e:
            raise NinjaError("loading deps log %s: %s" % log_path, e)

        if not self.config.dry_run:
            try:
                self.deps_log.OpenForWrite(log_path)
            except NinjaError as e:
                raise NinjaError("opening deps log: %s" % e)

    def RebuildManifest(self, input_file):
        path, slash_bits = CanonicalizePath(input_file)
        node = self.state.LookupNode(path)
        builder = Builder(self.state, self.config, self.build_log, self.deps_log, self.disk_interface)

        if not builder.AddTarget(node):
            return False

        if builder.AlreadyUpToDate():
            return False

        builder.Build()

        return node.dirty()

    def LookupTarget(self, path):
        path, slash_bits = CanonicalizePath(path)

        # Special syntax: "foo.cc^" means "the first output of foo.cc".
        if path.endswith("^"):
            path = path[:-1]
            first_dependent = True
        else:
            first_dependent = False

        node = self.state.LookupNode(path)

        if first_dependent:
            try:
                edge = node.out_edges()[0]
            except IndexError:
                raise NinjaError("'{}' has no out edge".format(path))

            try:
                node = edge.outputs_[0]
            except IndexError:
                edge.Dump()
                raise RuntimeError("edge has no outputs")

        return node

    def LookupTargets(self, paths):
        targets = []

        for path in paths:
            target = self.LookupTarget(path)

            if target is None:
                raise NinjaError("unknown target '%s'" % path)

            targets.append(target)

        return targets

    def RunBuild(self, paths=()):
        if len(paths) == 0:
            targets = self.state.DefaultNodes()
        else:
            targets = self.LookupTargets(paths)

        builder = Builder(self.state, self.config, self.build_log, self.deps_log, self.disk_interface)

        for target in targets:
            builder.AddTarget(target)

        if builder.AlreadyUpToDate():
            print("pyninja: no work to do.")
            return

        builder.Build()

    def IsPathDead(self, s):
        n = self.state.LookupNode(s)
        return (n is None or n.in_edge() is None) and self.disk_interface.Stat(s) == 0;

def config_from_options(options):
    config = BuildConfig()
    config.dry_run = options.dry_run
    config.parallelism = options.jobs
    config.failures_allowed = options.max_failures

    if options.show_commands:
        config.verbosity = BuildConfig.VERBOSE

    if options.max_load is not None:
        config.max_load_average = options.max_load

    if options.max_memory is not None:
        config.max_memory_usage = options.max_memory / 100

    return config

def set_debug_flags(flags):
    for flag in flags:
        if flag == "list":
            print("""debugging modes:
  explain  explain what caused a command to execute
  keeprsp  don't delete @response files on success""")
            if os.name == "nt":
                print("  nostatcache  don't batch stat() calls per directory and cache them")
            print("multiple modes can be enabled via -d FOO -d BAR")
            sys.exit(1)
        elif flag == "explain":
            cvar.explaining = True
        elif flag == "keeprsp":
            cvar.keep_rsp = True
        elif flag == "nostatcache":
            cvar.experimental_statcache = False

def main():
    from optparse import OptionParser
    default_job_count = multiprocessing.cpu_count() + 2
    option_parser = OptionParser(usage="%prog [options] targets", version="%prog " + version)
    option_parser.add_option("-C", dest="dir", help="change to DIR before doing anything else")
    option_parser.add_option("-f", dest="file", help="specify input build file [default=build.ninja]", default="build.ninja")
    option_parser.add_option("-j", dest="jobs", metavar="N", help="run N jobs in parallel [default=%d, derived from CPUs available]" % default_job_count,
                      default=default_job_count, type=int)
    option_parser.add_option("-l", dest="max_load", metavar="N", help="do not start new jobs if the load average is greater than N", type=float)
    option_parser.add_option("-m", dest="max_memory", metavar="N", help="do not start new jobs if the memory usage exceeds N percent", type=int)
    option_parser.add_option("-n", dest="dry_run", help="dry run (don't run commands but act like they succeeded)", action="store_true", default=False)
    option_parser.add_option("-k", dest="max_failures", help="keep going until N jobs fail [default=1]", type=int, default=1)
    option_parser.add_option("-v", dest="show_commands", help="show all command lines while building", action="store_true", default=False)
    option_parser.add_option("-d", dest="debug_flags", metavar="MODE", help="enable debugging (use -d list to list modes)",
                             action="append", choices=["explain", "keeprsp", "nostatcache", "list"], default=())

    options, targets = option_parser.parse_args()

    set_debug_flags(options.debug_flags)

    if options.dir is not None:
        print("pyninja: Entering directory `%s'" % options.dir);
        os.chdir(options.dir)

    config = config_from_options(options)

    # The build can take up to 2 passes: one to rebuild the manifest, then
    # another to build the desired target.
    for cycle in range(2):
        ninja = NinjaMain(config)

        parser = ManifestParser(ninja.state)

        try:
            parser.Load(options.file)
            ninja.EnsureBuildDirExists()
            ninja.OpenBuildLog()
            ninja.OpenDepsLog()

            if cycle == 0 and ninja.RebuildManifest(options.file):
                continue

            ninja.RunBuild(targets)
        except NinjaError as e:
            print("%s: error: %s" % (option_parser.get_prog_name(), e), file=sys.stderr)
            if "interrupted by user" in e.message:
                sys.exit(2)
            else:
                sys.exit(1)
        else:
            return

if __name__ == "__main__":
    main()
