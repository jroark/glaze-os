# App Store on AWS

The App Store is a static service: a catalog, icons and `.gzp` files. The AWS
backend consists of a private S3 bucket, CloudFront, an ACM certificate and DNS
records in the existing `glaze-os.com` Route 53 zone. No database, application
server, API Gateway, Lambda or developer accounts are needed.

## Infrastructure

`infra/appstore/stack.json` defines the `glaze-os-appstore` CloudFormation stack.
Deploy it in **us-east-1** because CloudFront requires its ACM certificate there.
The template creates:

- `glaze-os-appstore-<account-id>-us-east-1`: encrypted, versioned S3 storage,
  with public access blocked and ACLs disabled. The bucket is retained when
  the stack is deleted or replaced.
- A CloudFront distribution with Origin Access Control. Its signed HTTPS
  origin requests can read the bucket; anonymous S3 requests are denied.
- A DNS-validated RSA certificate for `appstore.glaze-os.com`, with automatic
  validation records in the existing zone, plus IPv4/IPv6 alias records.
- Separate cache policies: no CDN caching for the catalog; up to a year for
  immutable, versioned assets. Gzip and Brotli are disabled for both policies
  and automatic compression is off, matching the native streaming client.

The CDN accepts GET and HEAD, redirects HTTP to HTTPS, and permits TLS 1.2/1.3.
PriceClass_100 uses North American and European edge locations. Usage is billed
through S3 and CloudFront; the stack does not add paid application services.
The existing domain registration and hosted zone stay in place. The stack does
not manage the apex domain or other websites in the account.

## Deployed endpoint

- Catalog: `https://appstore.glaze-os.com/catalog.txt`
- Stack: `glaze-os-appstore` in `us-east-1`
- CloudFront distribution: `E39U3NJ10S25FF`
- S3 bucket: `glaze-os-appstore-000051858774-us-east-1`

The initial release contains five apps. All public objects and cache headers
were verified, and anonymous S3 access was confirmed denied. Use stack outputs
as the source of truth if these resources are replaced later.

Task Manager 1.0 was published as the sixth app on October 19, 2026. The
publisher verified the catalog, all packages/icons and cache headers over
public HTTPS, and confirmed that the S3 origin remains private. Task Manager's
CPU and task features need the updated kernel; older OS builds retain its
memory and uptime view. See [Task Manager](TASK_MANAGER.md).

## Create or update the stack

The working CLI login for the initial setup is the `linux4be` profile. Specify
it explicitly; the local default profile has unrelated, invalid credentials.
No credentials are stored in this repository. For another machine, use its
appropriate AWS profile instead.

```
aws --profile linux4be sts get-caller-identity
aws --profile linux4be cloudformation validate-template \
  --region us-east-1 --template-body file://infra/appstore/stack.json
aws --profile linux4be cloudformation deploy \
  --region us-east-1 --stack-name glaze-os-appstore \
  --template-file infra/appstore/stack.json \
  --parameter-overrides \
    HostedZoneId=Z0787879KYK5LHIR1IDN \
    DomainName=appstore.glaze-os.com \
  --no-fail-on-empty-changeset
```

The zone ID above is the existing public `glaze-os.com` zone. On another account,
look up its zone ID; do not create a duplicate zone. The nameservers at the
registrar must match this zone before ACM can validate the certificate.
CloudFront deployment and DNS validation can take several minutes. Inspect
progress and the resulting resource names with:

```
aws --profile linux4be cloudformation describe-stack-events \
  --region us-east-1 --stack-name glaze-os-appstore
aws --profile linux4be cloudformation describe-stacks \
  --region us-east-1 --stack-name glaze-os-appstore \
  --query 'Stacks[0].Outputs'
```

Keep the ACM validation CNAME in place for automatic certificate renewal.

## Publish a release

Build the catalog, validate it locally, then publish:

```
./build.sh store
python3 tools/publish_appstore.py --dry-run
python3 tools/publish_appstore.py --profile linux4be
```

The publisher reads the bucket and public URL from stack outputs. It validates
every package, icon, ID, version, size and SHA-256 before making any AWS call.
Only catalog-referenced files are uploaded; `_headers`, fixture files and
unrelated build outputs are excluded.

Existing immutable assets are downloaded and compared, and changed bytes at
an existing versioned path are rejected. New assets are uploaded with
conditional writes and verified from S3. Only after all assets are present is
`catalog.txt` replaced in a single conditional PUT. The catalog uses
`Cache-Control: no-cache, max-age=0, must-revalidate`; assets use
`Cache-Control: public, max-age=31536000, immutable`. No content encoding is set.
Concurrent changes to the catalog fail the conditional write; rerun after
checking the other release rather than overwriting it blindly. Older assets
are never deleted.

S3 versioning retains previous catalogs for rollback. To roll back, retrieve a
known-good earlier catalog version and publish a local release directory
containing that catalog and its matching assets through the same publisher.
Never remove assets referenced by older catalogs or installed clients.

After publishing, the tool fetches the catalog and every asset through the
public hostname and checks their bytes and cache headers. It also checks that
the S3 origin denies anonymous access. Repeat this read-only check at any time:

```
python3 tools/publish_appstore.py --profile linux4be --verify-only
```

If publication succeeds but DNS is still propagating, wait and rerun
`--verify-only`. There is no need to upload the files again. Since the catalog
cache policy has zero TTL, normal releases do not need CloudFront invalidations.
The default root URL serves the catalog too; this endpoint is the native store's
backend, without a separate web storefront.

## Native verification

The normal OS build already uses `https://appstore.glaze-os.com`. Boot a copy of
`out/glaze.bin` with `--rtc-host-time --ne2000`, open App Store, and install an app.
Use the usual build for release, without `APPSTORE_URL` overrides or scripted
input. A correct device date and compatible bundled trust roots are required.

Run offline publisher tests after changing release logic:

```
python3 tests/store_publisher.py
```

AWS documentation: [private S3 origins and OAC](https://docs.aws.amazon.com/AmazonCloudFront/latest/DeveloperGuide/private-content-restricting-access-to-s3.html),
[certificate region requirements](https://docs.aws.amazon.com/AmazonCloudFront/latest/DeveloperGuide/cnames-and-https-requirements.html),
[cache policy TTLs](https://docs.aws.amazon.com/AmazonCloudFront/latest/DeveloperGuide/cache-key-understand-cache-policy.html).
