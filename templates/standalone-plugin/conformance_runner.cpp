// Runs the SDK's conformance checks against one built plugin, named on the
// command line, and exits non-zero on any failure.
//
// This file is itself an example: copy it into your own plugin project as-is.
// It is a bare main() rather than a test framework because the harness is
// framework-agnostic by design — run_conformance() returns a report instead of
// asserting, so the same call works under Catch2, GoogleTest, or nothing at
// all. Depending on a framework here would mean an external dependency for a
// project whose whole point is that the SDK bundle is self-contained.
//
// One plugin per invocation, one ctest entry per plugin: a failure then names
// the plugin that broke instead of reporting that "the tests" failed.
//
// What this proves, and what it does not: conformance loads the plugin the way
// the host does — through the exported spc_plugin_vtable — and checks it
// answers the ABI honestly. It cannot tell whether the plugin does its job.
// That needs a behavioural test written against your own contract.

#include <testing/conformance.h>
#include <testing/plugin_under_test.h>

#include <cstdio>

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <path-to-plugin-module>\n", argv[0]);
        return 2;
    }
    const char* path = argv[1];

    spc::testing::PluginUnderTest plugin;
    if (!plugin.load(path)) {
        // A plugin that cannot be loaded at all is indistinguishable from one
        // that exports no vtable, so report the loader's own message.
        std::fprintf(stderr, "FAIL: %s\n", plugin.error().c_str());
        return 1;
    }

    const auto* d = plugin.descriptor();
    std::printf("plugin: %s (%s)\n", d && d->id[0] ? d->id : "<no id>", path);

    const auto report = spc::testing::run_conformance(plugin);
    std::printf("%s\n", report.summary().c_str());   // lists every failure
    return report.ok() ? 0 : 1;
}
