# GitLab CI connector fixtures

Everything here is **synthetic** (hence `*.synthetic.*`, see `fixtures/README.md`):
hand-written in the shapes GitLab's REST API v4 and GitLab Runner 17 produce,
with invented values. No GitLab instance, project, person or token produced
any of it. `gitlab.example.com`, `acme/mobile-app`, the user `dev.example`
and every SHA, id and timestamp are placeholders.

Used by `tests/unit/test_gitlab.cpp`, which replays them through a fake
`net::Transport` -- the same code path that parses live replies.

| File | Stands in for |
|---|---|
| `project.synthetic.json` | `GET /projects/:id` (a public project) |
| `user.synthetic.json` | `GET /user` |
| `projects_membership.synthetic.json` | `GET /projects?membership=true&simple=true` |
| `pipelines_page1.synthetic.json`, `pipelines_page2.synthetic.json` | `GET /projects/:id/pipelines`, two pages (`X-Next-Page: 2`, then empty) |
| `pipeline_<id>.synthetic.json` | `GET /projects/:id/pipelines/:pid` -- 1002 success on `main`, 1001 failed on a branch, 1000 canceled on a tag; each carries a `user` object, which must not reach a record |
| `jobs_<pid>.synthetic.json` | `GET /projects/:id/pipelines/:pid/jobs`; 1001 has a failed unit-test job (5011) and a failed `allow_failure` lint job (5012) |
| `trace_5011.synthetic.log` | `GET /projects/:id/jobs/5011/trace`: Gradle unit-test failure, with ANSI colours, section markers and a `\r` progress bar as the runner writes them |
| `trace_5012.synthetic.log` | the same for 5012: ESLint through `npm run lint`, npm 10 `npm error` style |
| `test_report_summary_<pid>.synthetic.json`, `test_report_summary_empty.synthetic.json` | `GET /projects/:id/pipelines/:pid/test_report_summary` |
| `deployments.synthetic.json` | `GET /projects/:id/deployments` |
| `gradle_failure.synthetic.log` | a Gradle dependency-resolution failure (root: the line after `* What went wrong:`) |
| `npm_failure.synthetic.log` | an `npm ci` ERESOLVE failure, npm 8/9 `npm ERR!` style |
| `compile_error.synthetic.log` | a Kotlin compile error under Gradle (root: the `e:` line, not Gradle's or the runner's summary) |
| `passing.synthetic.log` | a successful job whose text mentions "errors" and "failure" without failing |

The logs contain real escape bytes (`ESC`, `\r`); view them with `cat -v`.
