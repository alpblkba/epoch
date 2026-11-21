# Repository checks

Run the public terminology check from the repository root:

```sh
python3 scripts/check_public_terms.py
python3 scripts/test_check_public_terms.py
```

The checker scans public-candidate source files, including ignored implementation files in a full development worktree, for internal work-package labels and private-document citations. It reports its supported patterns; passing the check is not a substitute for manual review of technical accuracy or publication eligibility.
