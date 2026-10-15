# RE&CT tree and the 12-action pilot

The RE&CT incident-response framework (Apache-2.0,
<https://github.com/atc-project/atc-react>) is the second domain tree the engine
supports.  Decisions 0027 and 0029 define how.  The single source of truth for
its rules is [`../react.domain-profile.json`](../react.domain-profile.json);
[`pilot-react.campaign.json`](pilot-react.campaign.json) pins that profile by
hash and scopes the pilot to 12 response actions.

Stages (`S1`..`S6`), categories (`C0`..`C6`) and response actions (`RA####`)
are imported, never generated.  The engine expands each action into
environments, then ordered steps, then single operations, under the profile's
title rules and per-level child ranges.

## Run the pilot on a copy

Always on a copy of the data, never on a live node's data directory.  Replace
`<atc-react>` with a checkout of the RE&CT repository and `<copy>` with a fresh
scratch directory.

```bash
# 1. YAML -> dev-import CSVs (titles come from <atc-react>/docs/react.json)
data_depot react-convert --yaml-dir <atc-react>/response_actions --out <csv-dir>

# 2. import the tree into the copy, then publish the profile as a signed record
data_depot dev-import --root <csv-dir> --data-dir <copy> --tree react
data_depot profile add --file docs/react.domain-profile.json --data-dir <copy>
# the printed hash must equal "profile_hash" in pilot-react.campaign.json

# 3. record the campaign (every result goes to review: "review_all": true)
data_depot campaign start docs/react/pilot-react.campaign.json --data-dir <copy> --yes

# 4. run it; DEEPSEEK_API_KEY comes from the environment, never from a file
#    in the repo.  --budget-usd is the hard spend cap.
data_depot node run --base-url https://api.deepseek.com --model deepseek-flash \
  --api-port 18394 --budget-usd 1 \
  --price-in 0.30 --price-cache 0.006 --price-out 1.20 --data-dir <copy>
```

The campaign ends by itself when its 12 tickets are done; stop the node then.
Results are in the review queue (`data_depot review ...`); nothing is applied
to the tree until accepted.

Notes:

- The cost projection `campaign start` prints is shaped for the planning tree
  (it assumes every node expands a wide tree to depth 8).  For this pilot only
  its level-2 row applies (12 tickets); the real bounds are `loop_budget` and
  `--budget-usd`.
- `export-csv --tree react` exports the tree with a `sibling_order` column
  (steps in execution order); `export-csv --tree planning` exports the planning
  tree alone.
- Do not publish this profile to a fleet until every device runs a binary that
  includes decision 0029 (see its rollout section): older binaries accept the
  profile leniently and silently drop the ordering.
