# glaze-os.com

The project website is a static page in `site/`: `index.html`, `style.css`,
`unlock.js` (the hero's slide to unlock), `404.html`, Bitstream Vera from
`assets/fonts`, and images in `site/img`. It needs no build step to preview:

```
python3 -m http.server --directory site 8000
```

## Images

Everything on the device screens is a real Glaze screenshot. `tools/mksite.py`
builds `site/img` from the final screens of scripted runs (`tests/run.sh`
writes `out/shots/<name>.png`); its header lists the runs to capture. It also
needs the device frame artwork from the be300 repository
(`src/ui/assets/be300_frame.png`), and Pillow:

```
tools/mksite.py out/shots ../be300/src/ui/assets/be300_frame.png
```

The hero's lock screen is split into layers so the page can move the knob:
the screen with an empty track, the knob, the label and the label's mask
(for the shimmer). The empty track comes from `t_site_lockmid.txt` and
`t_site_lockdrag.txt`, which hold the knob at two places with the label faded.
The script checks it against both frames pixel for pixel. The track geometry
is SpringBoard's (`servers/springboard/sb_internal.h`); if it changes, update
`tools/mksite.py`, `site/style.css` and `site/unlock.js` together.

## Infrastructure

`infra/site/stack.json` defines the `glaze-os-site` CloudFormation stack, in
**us-east-1** because CloudFront needs its certificate there. It follows the
App Store's stack (`docs/APP_STORE_AWS.md`):

- `glaze-os-site-<account-id>-us-east-1`: encrypted, versioned S3 storage with
  public access blocked. Old object versions expire after 90 days. The bucket
  is retained if the stack is deleted.
- A CloudFront distribution with Origin Access Control, HTTP/2 and HTTP/3,
  compression, TLS 1.2 or later, and AWS's managed security-headers policy.
  It uses the managed CachingOptimized policy, which honours the objects'
  `Cache-Control`.
- A CloudFront Function that redirects `www.glaze-os.com` to the apex and
  serves `index.html` for directory URLs. Missing pages get `404.html`.
- One DNS-validated certificate for `glaze-os.com` and `www.glaze-os.com`, and
  A/AAAA alias records for both in the existing `glaze-os.com` zone.

The stack doesn't touch the App Store's `appstore.glaze-os.com` records.

```
aws --profile linux4be cloudformation deploy \
  --region us-east-1 --stack-name glaze-os-site \
  --template-file infra/site/stack.json \
  --parameter-overrides \
    HostedZoneId=Z0787879KYK5LHIR1IDN \
    DomainName=glaze-os.com \
  --no-fail-on-empty-changeset
```

Use `--no-execute-changeset` first to review a change set before applying it.
Certificate validation and the CloudFront deployment take several minutes.

## Publish

```
python3 tools/publish_site.py --profile linux4be --dry-run
python3 tools/publish_site.py --profile linux4be
```

The publisher reads the bucket and distribution from the stack's outputs. It
uploads assets with `Cache-Control: public, max-age=3600` and then the HTML
with `no-cache`, deleting files no longer in `site/`. It then invalidates the
distribution and checks the live site:
- the pages and frame image match `site/`
- missing pages get the 404 page
- `www` and plain HTTP redirect to `https://glaze-os.com`
- the bucket refuses anonymous reads

`--verify-only` repeats the checks without uploading.
