# Sentry fixtures

Replies in the shape of Sentry's public REST API (`/api/0/`), used by
`tests/unit/test_sentry.cpp` through a fake transport. Every value is
synthetic: the organization `acme`, project `mobile-app`, ids, users, commits
and URLs are invented, and no reply was recorded from a real account.

| File | Endpoint |
| --- | --- |
| `organization.json` | `GET /api/0/organizations/{org}/` |
| `projects.json` | `GET /api/0/organizations/{org}/projects/` |
| `project.json` | `GET /api/0/projects/{org}/{project}/` |
| `releases.json` | `GET /api/0/organizations/{org}/releases/` |
| `issues-page1.json`, `issues-page2.json` | `GET /api/0/projects/{org}/{project}/issues/` (two pages; the test adds the `Link` headers) |
| `event-<issue id>-latest.json` | `GET /api/0/organizations/{org}/issues/{id}/events/latest/` |

Issue `4970000003` has no latest event on purpose: the fake answers 404, as
Sentry does once an issue's events have aged out of retention.
