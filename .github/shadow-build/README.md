# Public shadow build

The [C++ workflow](../workflows/shadow-build.yml) accepts first-attempt
dispatches for C++ from the configured GitHub App on this repository's
protected `main`. It runs on standard Ubuntu, Windows, and macOS runners.
Other targets have their own dispatch workflows and source keys, and
check out this repository's builder script at an independently reviewed
commit. Standard hosted runner usage is free and unlimited in public
repositories, subject to GitHub Actions job and concurrency limits.
[GitHub-hosted runners](https://docs.github.com/en/actions/reference/runners/github-hosted-runners#standard-github-hosted-runners-for-public-repositories)

## Request

The caller reviews the complete export, computes the binary diff against
the exact public base, and encrypts it to the builder's age recipient. It
publishes one commit on `ci/preview/<request_id>` with the public base as
its parent and only the added ciphertext file `.haiven-shadow.age`. The
encrypted message begins with `haiven-shadow-diff:v1 <expected-tree>` and a
newline, followed by the binary Git patch.

The workflow accepts the positive decimal correlation ID as a
`request_id` input. Its `request` input is a JSON object with these fields.

| Property | Value |
| --- | --- |
| `request_id` | Same correlation ID as the workflow input |
| `target` | `cpp` in this workflow |
| `builder_sha` | Reviewed shared builder revision |
| `workflow_sha` | Reviewed revision of this workflow on protected `main` |
| `source` | Object with `base`, `carrier`, and ciphertext `sha256` |
| `dotnet` | Null in this workflow. The Unity core and Godot .NET workflows supply a companion diff. |
| `recipient` | Separate public age recipient controlled by the caller for encrypting build results |

## Environment

Set `SDK_SHADOW_BUILDER_SHA` to the reviewed common builder commit.
Set `SDK_SHADOW_DISPATCH_ACTOR_ID` to the allowed GitHub App bot's numeric
account ID. Add the source age identity as the environment secret
`SDK_SHADOW_SOURCE_AGE_IDENTITY` in the `shadow-source` environment. Restrict
the environment to `main` and require an independent deployment reviewer.
Disable administrative bypass. Keep the identity in this environment
and use a distinct identity for decrypting outputs. [GitHub environments](https://docs.github.com/en/actions/reference/workflows-and-actions/deployments-and-environments)

The reconstruction step verifies the exact public base, carrier and digest.
It applies the authenticated diff to a clean index and checks the expected
tree and regular-file modes before materializing the source. The build runs
after the decryption step exits. The Actions service and the assigned runner
can see reconstructed sources during execution. Public observers can see
metadata, timing, and ciphertext sizes.

## Results

The workflow has contents-read permissions and uses Actions artifacts for output.
Each platform uploads `shadow-<request_id>-<platform>` containing one
`build.age` file. The bundle encrypts the detailed diagnostics, compiled
payload, and a receipt binding the reconstruction, platform verdict, and
payload digest. The caller verifies run and artifact identities and the
decrypted receipt before retiring its shadow refs.
