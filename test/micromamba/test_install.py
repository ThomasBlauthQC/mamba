import json
import os
import platform
import random
import shutil
import string
import subprocess
from pathlib import Path

import pytest

from .helpers import *


class TestInstall:

    current_root_prefix = os.environ["MAMBA_ROOT_PREFIX"]
    current_prefix = os.environ["CONDA_PREFIX"]
    cache = os.path.join(current_root_prefix, "pkgs")

    env_name = random_string()
    root_prefix = os.path.expanduser(os.path.join("~", "tmproot" + random_string()))
    prefix = os.path.join(root_prefix, "envs", env_name)

    @classmethod
    def setup_class(cls):
        os.environ["MAMBA_ROOT_PREFIX"] = TestInstall.root_prefix
        os.environ["CONDA_PREFIX"] = TestInstall.prefix

        # speed-up the tests
        os.environ["CONDA_PKGS_DIRS"] = TestInstall.cache

        create("-n", TestInstall.env_name, "--offline", no_dry_run=True)

    @classmethod
    def teardown_class(cls):
        os.environ["MAMBA_ROOT_PREFIX"] = TestInstall.current_root_prefix
        os.environ["CONDA_PREFIX"] = TestInstall.current_prefix
        shutil.rmtree(TestInstall.root_prefix)

    @classmethod
    def teardown(cls):
        os.environ["MAMBA_ROOT_PREFIX"] = TestInstall.root_prefix
        os.environ["CONDA_PREFIX"] = TestInstall.prefix
        for v in ("CONDA_CHANNELS", "MAMBA_TARGET_PREFIX"):
            if v in os.environ:
                os.environ.pop(v)

        if Path(TestInstall.root_prefix).exists():
            remove("-n", "base", "-a", no_dry_run=True)

        if not Path(TestInstall.prefix).exists():
            create("-n", TestInstall.env_name, "--offline", no_dry_run=True)
        else:
            remove(
                "-n", TestInstall.env_name, "-a", no_dry_run=True,
            )

        res = umamba_list("xtensor", "-n", TestInstall.env_name, "--json")
        assert len(res) == 0

    @classmethod
    def config_tests(cls, res, root_prefix=root_prefix, target_prefix=prefix):
        assert res["root_prefix"] == root_prefix
        assert res["target_prefix"] == target_prefix
        assert res["use_target_prefix_fallback"]
        checks = (
            MAMBA_ALLOW_EXISTING_PREFIX
            | MAMBA_NOT_ALLOW_MISSING_PREFIX
            | MAMBA_NOT_ALLOW_NOT_ENV_PREFIX
            | MAMBA_EXPECT_EXISTING_PREFIX
        )
        assert res["target_prefix_checks"] == checks

    @pytest.mark.parametrize(
        "source,file_type",
        [
            ("cli_only", None),
            ("spec_file_only", "classic"),
            ("spec_file_only", "explicit"),
            ("spec_file_only", "yaml"),
            ("both", "classic"),
            ("both", "explicit"),
            ("both", "yaml"),
        ],
    )
    def test_specs(self, source, file_type):
        cmd = []
        specs = []

        if source in ("cli_only", "both"):
            specs = ["xframe", "xtl"]
            cmd = list(specs)

        if source in ("spec_file_only", "both"):
            f_name = random_string()
            spec_file = os.path.join(TestInstall.root_prefix, f_name)

            if file_type == "classic":
                file_content = ["xtensor >0.20", "xsimd"]
                specs += file_content
            elif file_type == "explicit":
                explicit_specs = [
                    "https://conda.anaconda.org/conda-forge/linux-64/xtensor-0.21.5-hc9558a2_0.tar.bz2#d330e02e5ed58330638a24601b7e4887",
                    "https://conda.anaconda.org/conda-forge/linux-64/xsimd-7.4.8-hc9558a2_0.tar.bz2#32d5b7ad7d6511f1faacf87e53a63e5f",
                ]
                file_content = ["@EXPLICIT"] + explicit_specs
                specs = explicit_specs
            else:  # yaml
                spec_file += ".yaml"
                file_content = ["dependencies:", "  - xtensor >0.20", "  - xsimd"]
                specs += ["xtensor >0.20", "xsimd"]

            with open(spec_file, "w") as f:
                f.write("\n".join(file_content))

            cmd += ["-f", spec_file]

        res = install(*cmd, "--print-config-only")

        TestInstall.config_tests(res)
        assert res["env_name"] == ""
        assert res["specs"] == specs

    @pytest.mark.parametrize("root_prefix", (None, "env_var", "cli"))
    @pytest.mark.parametrize("target_is_root", (False, True))
    @pytest.mark.parametrize("cli_prefix", (False, True))
    @pytest.mark.parametrize("cli_env_name", (False, True))
    @pytest.mark.parametrize("yaml", (False, True))
    @pytest.mark.parametrize("env_var", (False, True))
    @pytest.mark.parametrize("fallback", (False, True))
    def test_target_prefix(
        self,
        root_prefix,
        target_is_root,
        cli_prefix,
        cli_env_name,
        yaml,
        env_var,
        fallback,
    ):
        cmd = []

        if root_prefix in (None, "cli"):
            os.environ["MAMBA_DEFAULT_ROOT_PREFIX"] = os.environ.pop(
                "MAMBA_ROOT_PREFIX"
            )

        if root_prefix == "cli":
            cmd += ["-r", TestInstall.root_prefix]

        r = TestInstall.root_prefix

        if target_is_root:
            p = r
            n = "base"
        else:
            p = TestInstall.prefix
            n = TestInstall.env_name

        if cli_prefix:
            cmd += ["-p", p]

        if cli_env_name:
            cmd += ["-n", n]

        if yaml:
            f_name = random_string() + ".yaml"
            spec_file = os.path.join(TestInstall.prefix, f_name)

            file_content = [
                f"name: {n}",
                "dependencies: [xtensor]",
            ]
            with open(spec_file, "w") as f:
                f.write("\n".join(file_content))

            cmd += ["-f", spec_file]

        if env_var:
            os.environ["MAMBA_TARGET_PREFIX"] = p

        if not fallback:
            os.environ.pop("CONDA_PREFIX")
        else:
            os.environ["CONDA_PREFIX"] = p

        if ((cli_prefix or env_var) and (cli_env_name or yaml)) or not (
            cli_prefix or cli_env_name or yaml or env_var or fallback
        ):
            with pytest.raises(subprocess.CalledProcessError):
                install(*cmd, "--print-config-only")
        else:
            res = install(*cmd, "--print-config-only")
            TestInstall.config_tests(res, root_prefix=r, target_prefix=p)

    @pytest.mark.parametrize("cli", (False, True))
    @pytest.mark.parametrize("yaml", (False, True))
    @pytest.mark.parametrize("env_var", (False, True))
    @pytest.mark.parametrize("rc_file", (False, True))
    def test_channels(self, cli, yaml, env_var, rc_file):
        cmd = []
        expected_channels = []

        if cli:
            cmd += ["-c", "cli"]
            expected_channels += ["cli"]

        if yaml:
            f_name = random_string() + ".yaml"
            spec_file = os.path.join(TestInstall.prefix, f_name)

            file_content = [
                "channels: [yaml]",
                "dependencies: [xtensor]",
            ]
            with open(spec_file, "w") as f:
                f.write("\n".join(file_content))

            cmd += ["-f", spec_file]
            expected_channels += ["yaml"]

        if env_var:
            os.environ["CONDA_CHANNELS"] = "env_var"
            expected_channels += ["env_var"]

        if rc_file:
            f_name = random_string() + ".yaml"
            rc_file = os.path.join(TestInstall.prefix, f_name)

            file_content = ["channels: [rc]"]
            with open(rc_file, "w") as f:
                f.write("\n".join(file_content))

            cmd += ["--rc-file", rc_file]
            expected_channels += ["rc"]

        res = install(
            *cmd, "--print-config-only", no_rc=not rc_file, default_channel=False
        )
        TestInstall.config_tests(res)
        if expected_channels:
            assert res["channels"] == expected_channels
        else:
            assert res["channels"] is None

    @pytest.mark.parametrize("type", ("yaml", "classic", "explicit"))
    def test_multiple_spec_files(self, type):
        cmd = []
        specs = ["xtensor", "xsimd"]
        explicit_specs = [
            "https://conda.anaconda.org/conda-forge/linux-64/xtensor-0.21.5-hc9558a2_0.tar.bz2#d330e02e5ed58330638a24601b7e4887",
            "https://conda.anaconda.org/conda-forge/linux-64/xsimd-7.4.8-hc9558a2_0.tar.bz2#32d5b7ad7d6511f1faacf87e53a63e5f",
        ]

        for i in range(2):
            f_name = random_string()
            file = os.path.join(TestInstall.prefix, f_name)

            if type == "yaml":
                file += ".yaml"
                file_content = [f"dependencies: [{specs[i]}]"]
            elif type == "classic":
                file_content = [specs[i]]
                expected_specs = specs
            else:  # explicit
                file_content = ["@EXPLICIT", explicit_specs[i]]

            with open(file, "w") as f:
                f.write("\n".join(file_content))

            cmd += ["-f", file]

        if type == "yaml":
            with pytest.raises(subprocess.CalledProcessError):
                install(*cmd, "--print-config-only")
        else:
            res = install(*cmd, "--print-config-only")
            if type == "classic":
                assert res["specs"] == specs
            else:  # explicit
                assert res["specs"] == [explicit_specs[0]]

    @pytest.mark.parametrize("priority", (None, "disabled", "flexible", "strict"))
    @pytest.mark.parametrize("no_priority", (None, True))
    @pytest.mark.parametrize("strict_priority", (None, True))
    def test_channel_priority(self, priority, no_priority, strict_priority):
        cmd = ["-p", TestInstall.prefix, "xtensor"]
        expected_priority = "flexible"

        if priority:
            cmd += ["--channel-priority", priority]
            expected_priority = priority
        if no_priority:
            cmd += ["--no-channel-priority"]
            expected_priority = "disabled"
        if strict_priority:
            cmd += ["--strict-channel-priority"]
            expected_priority = "strict"

        if (
            (priority == "flexible")
            or (
                (priority is not None)
                and (
                    (no_priority and priority != "disabled")
                    or (strict_priority and priority != "strict")
                )
            )
            or (no_priority and strict_priority)
        ):
            with pytest.raises(subprocess.CalledProcessError):
                install(*cmd, "--print-config-only")
        else:
            res = install(*cmd, "--print-config-only")
            assert res["channel_priority"] == expected_priority

    def test_empty_specs(self):
        assert "Nothing to do." in install().strip()

    @pytest.mark.skipif(
        dry_run_tests is DryRun.ULTRA_DRY, reason="Running only ultra-dry tests"
    )
    @pytest.mark.parametrize("already_installed", [False, True])
    def test_non_explicit_spec(self, already_installed):
        cmd = ["-p", TestInstall.prefix, "xtensor", "--json"]

        if already_installed:
            install(*cmd, no_dry_run=True)

        res = install(*cmd)

        assert res["success"]
        assert res["dry_run"] == (dry_run_tests == DryRun.DRY)
        if already_installed:
            keys = {"dry_run", "success", "prefix", "message"}
            assert keys.issubset(set(res.keys()))
        else:
            keys = {"success", "prefix", "actions", "dry_run"}
            assert keys.issubset(set(res.keys()))

            action_keys = {"LINK", "PREFIX"}
            assert action_keys == set(res["actions"].keys())

            packages = {pkg["name"] for pkg in res["actions"]["LINK"]}
            expected_packages = {"xtensor", "xtl"}
            assert expected_packages.issubset(packages)

            if not dry_run_tests:
                pkg_name = get_concrete_pkg(res, "xtensor")
                orig_file_path = get_pkg(
                    pkg_name, xtensor_hpp, TestInstall.current_root_prefix
                )
                assert orig_file_path.exists()

    @pytest.mark.skipif(
        dry_run_tests is DryRun.ULTRA_DRY, reason="Running only ultra-dry tests"
    )
    @pytest.mark.parametrize("already_installed", [False, True])
    @pytest.mark.parametrize("valid", [False, True])
    def test_explicit_specs(self, already_installed, valid):
        spec_file_content = [
            "@EXPLICIT",
            "https://conda.anaconda.org/conda-forge/linux-64/xtensor-0.21.5-hc9558a2_0.tar.bz2#d330e02e5ed58330638a24601b7e4887",
        ]
        if not valid:
            spec_file_content += ["https://conda.anaconda.org/conda-forge/linux-64/xtl"]

        spec_file = os.path.join(TestInstall.root_prefix, "explicit_specs.txt")
        with open(spec_file, "w") as f:
            f.write("\n".join(spec_file_content))

        cmd = ("-p", TestInstall.prefix, "-q", "-f", spec_file)

        if valid:
            res = install(*cmd, default_channel=False)
            assert res.splitlines() == ["Linking xtensor-0.21.5-hc9558a2_0"]

            list_res = umamba_list("-p", TestInstall.prefix, "--json")
            assert len(list_res) == 1
            pkg = list_res[0]
            assert pkg["name"] == "xtensor"
            assert pkg["version"] == "0.21.5"
            assert pkg["build_string"] == "hc9558a2_0"
        else:
            with pytest.raises(subprocess.CalledProcessError):
                install(*cmd, default_channel=False)

    @pytest.mark.skipif(
        dry_run_tests is DryRun.ULTRA_DRY, reason="Running only ultra-dry tests"
    )
    @pytest.mark.parametrize(
        "alias",
        [
            "",
            "https://conda.anaconda.org/",
            "https://repo.mamba.pm/",
            "https://repo.mamba.pm",
        ],
    )
    def test_channel_alias(self, alias):
        if alias:
            res = install("xtensor", "--json", "--channel-alias", alias)
            ca = alias.rstrip("/")
        else:
            res = install("xtensor", "--json")
            ca = "https://conda.anaconda.org"

        for l in res["actions"]["LINK"]:
            assert l["channel"].startswith(f"{ca}/conda-forge/")
            assert l["url"].startswith(f"{ca}/conda-forge/")
