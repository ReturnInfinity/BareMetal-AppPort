// Added to package crypto/x509 by build-go-app.sh's -overlay. See GO.md.

package x509

// The CA bundle this port installs onto disk.img (port/mbedtls_port/
// install-cacert.sh) lives at /etc/ssl/cacert.pem, not at any of the paths
// Go's Linux root loader looks for. Package init runs before any
// certificate is verified, so appending here is enough.
func init() {
	certFiles = append(certFiles, "/etc/ssl/cacert.pem")
}
