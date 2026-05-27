# GitHub REST API — Repositories (Markdown snippet)

> Adapted from publicly documented GitHub REST API endpoints. Used for ChainAPI LLM feasibility testing only.

## Authentication

All requests require a Personal Access Token (PAT) in the `Authorization` header:

```
Authorization: Bearer <YOUR_PAT>
Accept: application/vnd.github+json
X-GitHub-Api-Version: 2022-11-28
```

There is no login endpoint; the token is pre-issued. To verify the token works, call `GET /user` and check for a 200.

## Get the authenticated user

```
GET https://api.github.com/user
```

Response 200:
```json
{
  "id": 1234,
  "login": "octocat",
  "name": "The Octocat"
}
```

## List repositories for the authenticated user

```
GET https://api.github.com/user/repos?per_page=30&sort=created
```

Response 200: array of repository objects.

## Create a repository for the authenticated user

```
POST https://api.github.com/user/repos
```

Request body (JSON):
```json
{
  "name": "my-repo",
  "private": false,
  "auto_init": true
}
```

Response 201:
```json
{
  "id": 5678,
  "full_name": "octocat/my-repo",
  "default_branch": "main"
}
```

## Get a repository

```
GET https://api.github.com/repos/{owner}/{repo}
```

`{owner}/{repo}` is the `full_name` returned at creation. Response 200.

## Delete a repository

```
DELETE https://api.github.com/repos/{owner}/{repo}
```

Requires the `delete_repo` scope on the PAT. Response 204.

## Create an issue

```
POST https://api.github.com/repos/{owner}/{repo}/issues
```

Request body (JSON):
```json
{
  "title": "Bug",
  "body": "Steps to reproduce",
  "labels": ["bug"]
}
```

Response 201:
```json
{
  "id": 9012,
  "number": 1,
  "state": "open"
}
```

The `number` is what you use to reference the issue in subsequent calls (not `id`).

## Comment on an issue

```
POST https://api.github.com/repos/{owner}/{repo}/issues/{issue_number}/comments
```

Request body:
```json
{ "body": "Comment text" }
```

Response 201.

## Close an issue

```
PATCH https://api.github.com/repos/{owner}/{repo}/issues/{issue_number}
```

Request body:
```json
{ "state": "closed" }
```

Response 200.
