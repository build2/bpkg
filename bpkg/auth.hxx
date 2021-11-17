// file      : bpkg/auth.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef BPKG_AUTH_HXX
#define BPKG_AUTH_HXX

#include <libbpkg/manifest.hxx>

#include <bpkg/types.hxx>
#include <bpkg/utility.hxx>

#include <bpkg/package.hxx>
#include <bpkg/common-options.hxx>

namespace bpkg
{
  // Authenticate a repository certificate. If the configuration directory is
  // NULL, then perform without a certificate database. Otherwise, use its
  // certificate database.
  //
  // If the dependent trust fingerprint is present then try to authenticate
  // the certificate for use by the dependent prior to prompting the user.
  // Note that if certificate is authenticated for such a use, then it is not
  // persisted into the database.
  //
  // If the configuration is used and also the configuration database is
  // specified, then assume the database is already opened with the
  // transaction started and use that. Otherwise, open the database and start
  // a new transaction.
  //
  // Note that one drawback of doing this as part of an existing transaction
  // is that if things go south and the transaction gets aborted, then all the
  // user's confirmations will be lost. For example, rep-fetch could fail
  // because it was unable to fetch some prerequisite repositories.
  //
  shared_ptr<const certificate>
  authenticate_certificate (const common_options&,
                            const dir_path* configuration,
                            database*,
                            const optional<string>& cert_pem,
                            const repository_location&,
                            const optional<string>& dependent_trust);

  // Authenticate a repository. First check that the certificate can be used
  // to authenticate this repository by making sure their names match. Then
  // recover the packages manifest file SHA256 checksum from the signature
  // and compare the calculated checksum to the recovered one.
  //
  // If the configuration directory is NULL, then create a temporary
  // certificate PEM file (cert_pem must be present). If the directory is
  // empty, then check if the current working directory is a configuration.
  // If it's not, then continue as if it was NULL (cert_pem must be present).
  // If it is, then continue as if a valid configuration directory was
  // specified. All other values (including '.') are assumed to be valid
  // configuration paths and will be diagnosed if that's not the case. In the
  // case of a valid configuration use the certificate PEM file from the
  // configuration (the file is supposed to have been created by the preceding
  // authenticate_certificate() call).
  //
  void
  authenticate_repository (const common_options&,
                           const dir_path* configuration,
                           const optional<string>& cert_pem,
                           const certificate&,
                           const signature_manifest&,
                           const repository_location&);

  // Sign a repository by calculating its packages manifest file signature.
  // This is done by encrypting the file's SHA256 checksum with the repository
  // certificate's private key and then base64-encoding the result. Issue
  // diagnstics and fail if the certificate has expired, and issue a warning
  // if it expires in less than 2 months. The repository argument is used for
  // diagnostics only.
  //
  // Note that currently we don't check if the key matches the certificate. A
  // relatively easy way to accomplish this would be to execute the following
  // commands and match the results:
  //
  // openssl x509 -noout -modulus -in cert.pem
  // openssl rsa -noout -modulus -in key.pem
  //
  // However, it would require to enter the key password again, which is a
  // showstopper. Maybe the easiest would be to recover the sum back from the
  // signature using the certificate, and compare it with the original sum
  // (like we do in authenticate_repository()). But that would require to
  // temporarily save the certificate to file.
  //
  std::vector<char>
  sign_repository (const common_options&,
                   const string& sha256sum,
                   const string& key_name, // --key option value
                   const string& cert_pem,
                   const dir_path& repository);

  // Parse a repository certificate. The repository location argument is used
  // for diagnostics only.
  //
  shared_ptr<certificate>
  parse_certificate (const common_options&,
                     const string& cert_pem,
                     const repository_location&);
}

#endif // BPKG_AUTH_HXX
