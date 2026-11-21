# Public shadow build

Dispatch [the workflow](../workflows/shadow-build.yml) on this
repository's protected `main` using a caller authorized for Actions write
access. It accepts `dotnet`, `unity-core`, `godot-dotnet`, `godot-demo`,
`cpp`, and `cli`. Managed targets use Ubuntu. Platform builds use Ubuntu,
Windows, and macOS standard runners. Usage on these public standard runners
is free and unlimited, subject to GitHub Actions job and concurrency limits.
[GitHub-hosted runners](https://docs.github.com/en/actions/reference/runners/github-hosted-runners#standard-github-hosted-runners-for-public-repositories)

## Request

The caller approves the exact public commits before dispatch, records the
workflow revision and run ID, and retains control over the source refs. All
source revisions must be accessible in their designated public repositories.
The `request_id` workflow input is a positive decimal correlation ID.
The `request` input is a JSON object with these string properties.

| Property | Value |
| --- | --- |
| `request_id` | Same correlation ID as the workflow input |
| `target` | One of the supported target names above |
| `source_sha` | Exact 40-character source commit |
| `dotnet_sha` | Exact public .NET commit for `unity-core` and `godot-dotnet`, empty for other targets |
| `version` | The `cli` version in its public `version.txt`, empty for other targets |
| `recipient` | Public X25519 age recipient controlled by the caller |

## Results

Each platform uploads `shadow-<request_id>-<platform>` containing one
`build.age` file. Detailed build logs and output are encrypted together
with an identity receipt that includes the source revisions, platform,
verdict, and successful payload digest. The public run displays metadata
and a build verdict. Commit contents are public and may persist in
downstream copies after the caller retires its temporary refs.

The caller verifies the exact run, workflow and commit, platform artifact IDs
and download digests, and the decrypted receipt before trusting results.
Keep the age identity in the caller's trusted environment.
