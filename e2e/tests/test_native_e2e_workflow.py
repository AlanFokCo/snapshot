# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

"""Exercise the native workflow's inputs and existing required-case guard on CPU."""

from __future__ import annotations

import ast
import os
import subprocess
from pathlib import Path

import pytest
import yaml

ROOT = Path(__file__).resolve().parents[2]
REQUIRED_ENV = "SNAPSHOT_E2E_REQUIRED_TESTS"
CASES = {
    "Run snapshot lifecycle tests": (
        "test_snapshot_lifecycle.py",
        {
            "test_successful_snapshot_captures_cpu_gpu_and_fs",
            "test_snapshot_records_the_environment_a_restore_is_checked_against",
            "test_successful_restore_recovers_cpu_gpu_and_fs_from_snapshot",
        },
    ),
    "Run SnapshotJob lifecycle tests": (
        "test_snapshotjob.py",
        {"test_snapshotjob_captures_and_restore_recovers_state"},
    ),
}


def workflow():
    return yaml.load(
        (ROOT / ".github/workflows/e2e.yaml").read_text(), Loader=yaml.BaseLoader
    )


def step_named(job, name):
    return next(
        step for step in workflow()["jobs"][job]["steps"] if step.get("name") == name
    )


@pytest.mark.parametrize("tag", ["", "a", "_tag", "v1.2.3-rc.1", "a" * 128])
def test_valid_inputs_reach_the_existing_consumer(tmp_path, tag):
    validate = step_named("validate", "Validate Snapshot image tag")
    env = {**os.environ, "SNAPSHOT_TAG": tag}
    result = subprocess.run(
        ["bash", "-eu", "-c", validate["run"]],
        env=env,
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0, result.stderr
    consumer = step_named("e2e", "Use provided Snapshot image tag")
    resolver = step_named("e2e", "Resolve Snapshot image tag")
    assert consumer["if"] == "${{ (inputs.snapshot_tag || '') != '' }}"
    assert resolver["if"] == "${{ (inputs.snapshot_tag || '') == '' }}"
    assert "--fallback-main" not in resolver["run"]
    if tag:
        env_file = tmp_path / "github-env"
        result = subprocess.run(
            ["bash", "-c", consumer["run"]],
            env={**env, "GITHUB_ENV": str(env_file)},
            capture_output=True,
            text=True,
            check=False,
        )
        assert result.returncode == 0, result.stderr
        assert env_file.read_text() == f"SNAPSHOT_E2E_SNAPSHOT_TAG={tag}\n"
        assert result.stdout == f"Using provided Snapshot image tag: {tag}\n"


@pytest.mark.parametrize(
    "tag",
    [
        " tag",
        "tag ",
        "tag\n",
        "tag\nOTHER=value",
        "tag\rvalue",
        "tag value",
        "$(printf injected)",
        "`printf injected`",
        'tag"value',
        "repo:tag",
        "image@sha256:" + "a" * 64,
        "-tag",
        ".tag",
        "täg",
        "a" * 129,
    ],
)
def test_invalid_inputs_fail_the_actual_validation_step(tag):
    step = step_named("validate", "Validate Snapshot image tag")
    result = subprocess.run(
        ["bash", "-eu", "-c", step["run"]],
        env={**os.environ, "SNAPSHOT_TAG": tag},
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode != 0
    assert "Snapshot image tag must be" in result.stderr
    assert not result.stdout


def test_validation_precedes_gpu_allocation_and_inputs_are_data():
    data = workflow()
    validate = data["jobs"]["validate"]
    assert validate["runs-on"] == "ubuntu-latest"
    assert validate["timeout-minutes"] == "2"
    assert validate["permissions"] == {}
    assert all("uses" not in step for step in validate["steps"])
    assert data["jobs"]["e2e"]["needs"] == "validate"
    assert data["jobs"]["e2e"]["runs-on"] == "prod-deploy-tester-v1"
    assert (
        step_named("validate", "Validate Snapshot image tag")["env"]["SNAPSHOT_TAG"]
        == "${{ inputs.snapshot_tag || '' }}"
    )
    assert (
        step_named("e2e", "Use provided Snapshot image tag")["env"]["SNAPSHOT_TAG"]
        == "${{ inputs.snapshot_tag }}"
    )
    for job, name in (
        ("validate", "Validate Snapshot image tag"),
        ("e2e", "Use provided Snapshot image tag"),
    ):
        assert "${{" not in step_named(job, name)["run"]
    assert set(data["on"]) == {"workflow_dispatch", "schedule"}
    assert data["permissions"] == {"contents": "read", "packages": "read"}
    assert step_named("e2e", "Cleanup e2e vCluster")["if"] == "${{ always() }}"


@pytest.mark.parametrize("step_name", CASES)
def test_required_names_match_the_real_consumer(step_name):
    filename, expected = CASES[step_name]
    step = step_named("e2e", step_name)
    names = {name.strip() for name in step["env"][REQUIRED_ENV].split(",")}
    assert names == expected
    assert f"pytest e2e/tests/{filename}" in step["run"]
    source = ast.parse((ROOT / "e2e/tests" / filename).read_text())
    functions = {node.name for node in source.body if isinstance(node, ast.FunctionDef)}
    assert names <= functions


@pytest.mark.parametrize("step_name", CASES)
@pytest.mark.parametrize("outcome", ["pass", "skip", "fail", "missing", "deselected"])
def test_workflow_required_cases_use_the_real_pytest_guard(
    pytester, monkeypatch, step_name, outcome
):
    step = step_named("e2e", step_name)
    monkeypatch.setenv(REQUIRED_ENV, step["env"][REQUIRED_ENV])
    names = sorted(CASES[step_name][1])
    pytester.makeconftest((ROOT / "e2e/tests/conftest.py").read_text())
    functions = ["import pytest"]
    for index, name in enumerate(names):
        if index == 0 and outcome == "missing":
            continue
        body = "pass"
        if index == 0 and outcome == "skip":
            body = "pytest.skip('GPU unavailable')"
        elif index == 0 and outcome == "fail":
            body = "assert False, 'restore failed'"
        functions.append(f"def {name}():\n    {body}\n")
    functions.append("def test_unrelated():\n    pass\n")
    pytester.makepyfile(test_inner="\n".join(functions))
    args = ["-p", "no:cacheprovider"]
    if outcome == "deselected":
        args.extend(["-k", f"not {names[0]}"])
    run = pytester.runpytest_inprocess(*args)
    if outcome == "pass":
        run.assert_outcomes(passed=len(names) + 1)
        assert run.ret == pytest.ExitCode.OK
    else:
        assert run.ret != pytest.ExitCode.OK
        if outcome in {"skip", "fail"}:
            run.assert_outcomes(failed=1, passed=len(names))
        else:
            run.stderr.fnmatch_lines(["*did not select:*"])


def test_static_ci_runs_this_regression_suite():
    assert (
        "e2e/tests/test_native_e2e_workflow.py"
        in (ROOT / ".github/workflows/ci.yml").read_text()
    )
