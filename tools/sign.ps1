# Self-signed Authenticode signing for local alpha builds (never for piper/onnxruntime).
# The private key is non-exportable and stays in Cert:\CurrentUser\My. -Trust is manual and is NOT run by CI/release:
# it adds the public certificate to CurrentUser\Root (Windows shows a confirmation dialog) so this machine trusts it.
param(
    [Parameter(Mandatory)][string[]]$Path,
    [switch]$Trust
)
$ErrorActionPreference = 'Stop'
$subject = 'CN=Ebook Reader (self-signed alpha)'
$signtool = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe'
$allowed = @('EbookReader.exe', 'eb_mupdf.dll')
if (-not (Test-Path $signtool)) { throw "signtool not found: $signtool" }

$cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert | Where-Object { $_.Subject -eq $subject -and $_.NotAfter -gt (Get-Date) } |
    Sort-Object NotAfter -Descending | Select-Object -First 1
if (-not $cert) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject -KeyExportPolicy NonExportable `
        -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(2)
}
if ($Trust) {
    $cer = Join-Path $env:TEMP 'eb-alpha-public.cer'
    Export-Certificate -Cert $cert -FilePath $cer | Out-Null
    Import-Certificate -FilePath $cer -CertStoreLocation Cert:\CurrentUser\Root | Out-Null
    Remove-Item $cer
}

$bad = 0
foreach ($p in $Path) {
    $f = Get-Item -LiteralPath $p
    if ($allowed -notcontains $f.Name) { throw "refusing to sign third-party or unknown file: $($f.Name)" }
    & $signtool sign /sha1 $cert.Thumbprint /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 $f.FullName | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "signtool failed for $($f.Name)" }
    $sig = Get-AuthenticodeSignature -LiteralPath $f.FullName
    $ok = $sig.SignerCertificate -and $sig.SignerCertificate.Thumbprint -eq $cert.Thumbprint -and
        $sig.Status -in @('Valid', 'UnknownError') -and $sig.TimeStamperCertificate
    if (-not $ok) { $bad++ }
    Write-Output ("SIGN {0} {1} {2} ts={3}" -f $(if ($ok) { 'OK' } else { 'FAIL' }), $f.Name, $sig.Status, [bool]$sig.TimeStamperCertificate)
}
if ($bad) { exit 1 }
