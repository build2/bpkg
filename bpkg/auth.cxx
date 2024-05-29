// file      : bpkg/auth.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <bpkg/auth.hxx>

#include <ratio>
#include <limits>   // numeric_limits
#include <iterator> // ostreambuf_iterator

#include <libbutl/base64.hxx>
#include <libbutl/openssl.hxx>
#include <libbutl/timestamp.hxx>
#include <libbutl/filesystem.hxx>
#include <libbutl/semantic-version.hxx>

#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/diagnostics.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
  static const string openssl_version_cmd ("version");
  static const string openssl_pkeyutl_cmd ("pkeyutl");
  static const string openssl_rsautl_cmd  ("rsautl");
  static const string openssl_x509_cmd    ("x509");

  const char* openssl_commands[5] = {openssl_version_cmd.c_str (),
                                     openssl_pkeyutl_cmd.c_str (),
                                     openssl_rsautl_cmd.c_str (),
                                     openssl_x509_cmd.c_str (),
                                     nullptr};

  // Print process command line.
  //
  static void
  print_command (const char* const args[], size_t n)
  {
    if (verb >= 2)
      print_process (args, n);
  }

  // Query the openssl information and return the openssl version. Cache the
  // version on the first function call. Fail on the underlying process and IO
  // error. Return the 0.0.0 version if unable to parse the openssl stdout.
  //
  static optional<semantic_version> openssl_ver;

  static const semantic_version&
  openssl_version (const common_options& co)
  {
    const path& openssl_path (co.openssl ()[openssl_version_cmd]);

    if (!openssl_ver)
    try
    {
      optional<openssl_info> oi (
        openssl::info (print_command, 2, openssl_path));

      openssl_ver = (oi && oi->name == "OpenSSL"
                     ? move (oi->version)
                     : semantic_version ());
    }
    catch (const process_error& e)
    {
      fail << "unable to execute " << openssl_path << ": " << e << endf;
    }
    catch (const io_error& e)
    {
      fail << "unable to read '" << openssl_path << "' output: " << e
           << endf;
    }

    return *openssl_ver;
  }

  // Return true if the openssl version is greater or equal to 3.0.0 and so
  // pkeyutl needs to be used instead of rsautl.
  //
  // Note that openssl 3.0.0 deprecates rsautl in favor of pkeyutl.
  //
  // Also note that pkeyutl is only implemented in openssl version 1.0.0 and
  // its -verifyrecover mode is broken in the [1.1.1 1.1.1d] version range
  // (see the 'pkeyutl -verifyrecover error "input data too long to be a
  // hash"' issue report for details).
  //
  static inline bool
  use_openssl_pkeyutl (const common_options& co)
  {
    return openssl_version (co) >= semantic_version {3, 0, 0};
  }

  // Return true if some openssl commands (openssl x509 -fingerprint, etc) may
  // issue the 'Reading certificate from stdin since no -in or -new option is
  // given' warning. This is the case for the openssl version in the [3.2.0
  // 3.2.2) range (see GH issue #353 for details).
  //
  // Note that there is no easy way to suppress this warning on Windows and
  // thus we don't define this function there.
  //
#ifndef _WIN32
  static inline bool
  openssl_warn_stdin (const common_options& co)
  {
    // Use 3.2.3 in the comparison rather than 3.2.2, to make sure that, for
    // example, 3.2.2-dev (denotes a pre-release of 3.2.2) also falls into the
    // range.
    //
    const semantic_version& v (openssl_version (co));
    return v >= semantic_version {3, 2, 0} && v < semantic_version {3, 2, 3};
  }
#endif

  // Find the repository location prefix that ends with the version component.
  // We consider all repositories under this location to be related.
  //
  static string
  name_prefix (const repository_location& rl)
  {
    assert (rl.absolute () || rl.remote ());

    // Construct the prefix as a relative repository location.
    //
    dir_path p;
    for (auto i (rl.path ().rbegin ()), e (rl.path ().rend ()); i != e; ++i)
    {
      const string& c (*i);
      if (!c.empty () && c.find_first_not_of ("1234567890") == string::npos)
        break;

      p /= "..";
    }

    p /= ".";

    // If this is a remote location then use the canonical name prefix. For a
    // local location this doesn't always work. Consider:
    //
    // .../pkg/1/build2.org/common/hello
    //
    // In this case we will end with an empty canonical name (because of the
    // special pkg/1 treatment). So in case of local locations we will use the
    // location rather than the name prefix.
    //
    if (rl.remote ())
      return repository_location (
        repository_url (p.posix_string ()),
        repository_type::pkg,
        rl).canonical_name ();
    else
      return (path_cast<dir_path> (rl.path ()) / p).normalize ().string ();
  }

  // Authenticate a dummy certificate. If trusted, it will authenticate all
  // the (unsigned) repositories under the location prefix of up-to-the-
  // version component.
  //
  static shared_ptr<certificate>
  auth_dummy (const common_options& co,
              const string& fp,
              const repository_location& rl)
  {
    tracer trace ("auth_dummy");

    shared_ptr<certificate> cert (
      make_shared<certificate> (fp, name_prefix (rl)));

    l4 ([&]{trace << "new cert: " << *cert;});

    if (co.trust_yes ())
    {
      if (verb >= 2)
        info << "unsigned repository " << rl.canonical_name () <<
          " trusted by command line";
    }
    else
    {
      (co.trust_no ()
       ? error
       : warn) << "repository " << rl.canonical_name () << " is unsigned";
    }

    if (co.trust_no () ||
        (!co.trust_yes () &&
         !yn_prompt (
           string ("continue without authenticating repositories at " +
                   cert->name + "? [y/n]").c_str ())))
      throw failed ();

    return cert;
  }

  // Calculate the real repository certificate fingerprint.
  //
  struct fingerprint
  {
    string canonical;   // Canonical representation.
    string abbreviated; // No colons, lower case, first 16 chars only.
  };

  static fingerprint
  real_fingerprint (const common_options& co,
                    const string& pem,
                    const repository_location& rl)
  {
    tracer trace ("real_fingerprint");

    auto calc_failed = [&rl] (const exception* e = nullptr)
    {
      diag_record dr (error);
      dr << "unable to calculate certificate fingerprint for "
         << rl.canonical_name ();

      if (e != nullptr)
        dr << ": " << *e;
    };

    const path& openssl_path (co.openssl ()[openssl_x509_cmd]);
    const strings& openssl_opts (co.openssl_option ()[openssl_x509_cmd]);

    try
    {
      openssl os (print_command,
                  fdstream_mode::text, fdstream_mode::text, 2,
                  openssl_path, openssl_x509_cmd,
                  openssl_opts,
                  "-sha256",
                  "-noout",
                  "-fingerprint"
#ifndef _WIN32
                  ,
                  (openssl_warn_stdin (co)
                   ? cstrings ({"-in", "/dev/stdin"})
                   : cstrings ())
#endif
      );

      os.out << pem;
      os.out.close ();

      string s;
      getline (os.in, s);
      os.in.close ();

      if (os.wait ())
      {
        // Normally the output is:
        //
        //  SHA256 Fingerprint=<fingerprint>
        //
        // But it can be translated and SHA spelled in lower case (LC_ALL=C
        // doesn't seem to help in some cases).
        //
        if (icasecmp (s, "SHA256", 6) == 0)
        {
          size_t p (s.find ('='));
          if (p != string::npos)
          {
            try
            {
              string fp (s, p + 1);
              string ab (fingerprint_to_sha256 (fp, 16));
              return {move (fp), move (ab)};
            }
            catch (const invalid_argument&)
            {
            }
          }
        }
      }

      calc_failed ();

      // Fall through.
    }
    catch (const process_error& e)
    {
      error << "unable to execute " << openssl_path << ": " << e;

      // Fall through.
    }
    catch (const io_error& e)
    {
      calc_failed (&e);

      // Fall through.
    }

    throw failed ();
  }

  // Calculate the repository certificate fingerprint. For dummy certificate
  // only the abbreviated form is meaningful (see certificate class definition
  // for details).
  //
  static fingerprint
  cert_fingerprint (const common_options& co,
                    const optional<string>& pem,
                    const repository_location& rl)
  {
    return pem
      ? real_fingerprint (co, *pem, rl)
      : fingerprint {string (),
                     sha256 (name_prefix (rl)).abbreviated_string (12)};
  }

  // Parse the PEM-encoded certificate representation.
  //
  static shared_ptr<certificate>
  parse_cert (const common_options& co,
              const fingerprint& fp,
              const string& pem,
              const string& repo)
  {
    tracer trace ("parse_cert");

    auto parse_failed = [&repo] (const exception* e = nullptr)
    {
      diag_record dr (error);
      dr << "unable to parse certificate for " << repo;

      if (e != nullptr)
        dr << ": " << *e;
    };

    const path& openssl_path (co.openssl ()[openssl_x509_cmd]);
    const strings& openssl_opts (co.openssl_option ()[openssl_x509_cmd]);

    try
    {
      // The order of the options we pass to openssl determines the order in
      // which we get things in the output. And want we expect is this
      // (leading space added):
      //
      // subject=
      //     CN=name:cppget.org
      //     O=Code Synthesis
      // notBefore=Apr  7 12:20:58 2016 GMT
      // notAfter=Apr  7 12:20:58 2017 GMT
      // info@cppget.org
      //
      // The first line must be "subject=" (it cannot be omitted from the
      // cert). After it we have one or more lines indented with four spaces
      // that specify the components. We are interested in CN and O, though
      // there could be others which we ignore. Then we must have the
      // notBefore and notAfter dates, again they presumably must be there.
      // The final line should be the email but will be silently missing if
      // the cert has no email.
      //
      openssl os (
        print_command,
        fdstream_mode::text, fdstream_mode::text, 2,
        openssl_path, openssl_x509_cmd,
        openssl_opts, "-noout", "-subject", "-dates", "-email",

        // Previously we have used "RFC2253,sep_multiline" format to display
        // the requested fields, but that resulted in some undesirable
        // behavior like escaping commas (\,) while dispaying only one field
        // per line. The reason for that is RFC2253 specifier which get
        // expanded into:
        //
        // esc_2253,esc_ctrl,esc_msb,utf8,dump_nostr,dump_unknown,dump_der,
        // sep_comma_plus,dn_rev,sname.
        //
        // Now we filtered them and leave just those specifiers that we
        // really need:
        //
        // utf8          - use UTF8 encoding for strings;
        //
        // esc_ctrl      - display control characters in \XX notation (we
        //                 don't expect them in properly created
        //                 certificates, but it's better to print this way if
        //                 they appear);
        //
        // sname         - use short form for field names (like
        //                 "O=Code Synthesis" vs
        //                 "organizationName=Code Synthesis");
        //
        // dump_nostr    - do not print any binary data in the binary form;
        // dump_der
        //
        // sep_multiline - display field per line.
        //
        "-nameopt", "utf8,esc_ctrl,dump_nostr,dump_der,sname,sep_multiline"

#ifndef _WIN32
        ,
        (openssl_warn_stdin (co)
         ? cstrings ({"-in", "/dev/stdin"})
         : cstrings ())
#endif
      );

      // We unset failbit to provide the detailed error description (which
      // certificate field is missed) on failure.
      //
      os.in.exceptions (ifdstream::badbit);

      // Reading from and writing to the child process standard streams from
      // the same thread is generally a bad idea. Depending on the program
      // implementation we can block on writing if the process input pipe
      // buffer get filled. That can happen if the process do not read
      // anymore, being blocked on writing to the filled output pipe, which
      // get filled not being read on the other end.
      //
      // Fortunatelly openssl reads the certificate before performing any
      // output.
      //
      os.out << pem;
      os.out.close ();

      try
      {
        auto bad_cert ([](const string& d) {throw invalid_argument (d);});

        auto get = [&os, &trace] (string& s) -> bool
        {
          bool r (getline (os.in, s));
          l6 ([&]{trace << s;});
          return r;
        };

        string s;
        if (!get (s) || s.compare (0, 8, "subject=") != 0)
          bad_cert ("no subject");

        // Parse RDN (relative distinguished name).
        //
        auto parse_rdn = [&s, &bad_cert] (size_t o, const char* name) -> string
        {
          string r (s.substr (o));
          if (r.empty ())
            bad_cert (name + string (" is empty"));

          return r;
        };

        auto parse_date = [&s](size_t o, const char* name) -> timestamp
        {
          // Certificate validity dates are internally represented as ASN.1
          // GeneralizedTime and UTCTime
          // (https://www.ietf.org/rfc/rfc4517.txt). While GeneralizedTime
          // format allows fraction of a second to be specified, the x.509
          // Certificate specification (https://www.ietf.org/rfc/rfc5280.txt)
          // do not permit them to be included into the validity dates. These
          // dates are printed by openssl in the 'MON DD HH:MM:SS[ GMT]'
          // format. MON is a month abbreviated name (C locale), timezone is
          // either GMT or absent (means local time). Examples:
          //
          // Apr 11 10:20:02 2016 GMT
          // Apr 11 10:20:02 2016
          //
          // We will require the date to be in GMT, as generally can not
          // interpret the certificate origin local time. Note:
          // openssl-generated certificate dates are always in GMT.
          //
          try
          {
            // Assume the global locale is not changed, and still "C".
            //
            const char* end;
            timestamp t (from_string (
                           s.c_str () + o, "%b %d %H:%M:%S %Y", false, &end));

            if (strcmp (end, " GMT") == 0)
              return t;
          }
          catch (const system_error&)
          {
          }

          throw invalid_argument ("invalid " + string (name) + " date");
        };

        string name;
        string org;
        while (get (s))
        {
          if (s.compare (0, 7, "    CN=") == 0)
            name = parse_rdn (7, "common name");
          else if (s.compare (0, 6, "    O=") == 0)
            org = parse_rdn (6, "organization name");
          else if (s.compare (0, 4, "    ") != 0)
            break; // End of the subject sub-lines.
        }

        if (name.empty ())
          bad_cert ("no common name (CN)");

        if (name.compare (0, 5, "name:") != 0)
          bad_cert ("no 'name:' prefix in the common name (CN)");

        name = name.substr (5);
        if (name.empty ())
          bad_cert ("no repository name in the common name (CN)");

        if (org.empty ())
          bad_cert ("no organization name (O)");

        if (!os.in || s.compare (0, 10, "notBefore=") != 0)
          bad_cert ("no start date");

        timestamp not_before (parse_date (10, "start"));

        if (!get (s) || s.compare (0, 9, "notAfter=") != 0)
          bad_cert ("no end date");

        timestamp not_after (parse_date (9, "end"));

        if (not_before >= not_after)
          bad_cert ("invalid date range");

        string email;
        if (!get (email) || email.empty ())
          bad_cert ("no email");

        // Ensure no data left in the stream.
        //
        if (os.in.peek () != ifdstream::traits_type::eof ())
          bad_cert ("unexpected data");

        os.in.close ();

        shared_ptr<certificate> cert (
          make_shared<certificate> (
            fp.abbreviated,
            fp.canonical,
            move (name),
            move (org),
            move (email),
            move (not_before),
            move (not_after)));

        if (os.wait ())
          return cert;

        // Fall through.
        //
      }
      catch (const invalid_argument& e)
      {
        // If the child exited with an error status, then omit any output
        // parsing diagnostics since we were probably parsing garbage.
        //
        if (os.wait ())
          fail << "invalid certificate for " << repo << ": " << e << endf;

        // Fall through.
      }

      parse_failed ();

      // Fall through.
    }
    catch (const process_error& e)
    {
      error << "unable to execute " << openssl_path << ": " << e;

      // Fall through.
    }
    catch (const io_error& e)
    {
      parse_failed (&e);

      // Fall through.
    }

    throw failed ();
  }

  // Verify the certificate (validity period and such).
  //
  static void
  verify_cert (const certificate& cert, const repository_location& rl)
  {
    if (!cert.dummy ())
    {
      if (cert.expired ())
        fail << "certificate for repository " << rl.canonical_name ()
             << " has expired";
    }
  }

  // Authenticate a real certificate. Return the authenticated certificate and
  // flag if it was authenticated by the user (via the command line/prompt) or
  // by the dependent trust.
  //
  struct cert_auth
  {
    shared_ptr<certificate> cert;
    bool user;
  };

  static cert_auth
  auth_real (const common_options& co,
             const fingerprint& fp,
             const string& pem,
             const repository_location& rl,
             const optional<string>& dependent_trust)
  {
    tracer trace ("auth_real");

    shared_ptr<certificate> cert (
      parse_cert (co, fp, pem, rl.canonical_name ()));

    l4 ([&]{trace << "new cert: " << *cert;});

    verify_cert (*cert, rl);

    // @@ Is there a way to intercept CLI parsing for the specific option of
    // the standard type to validate/convert the value? If there were, we could
    // validate the option value converting fp to sha (internal representation
    // of fp).
    //
    // @@ Not easily/cleanly. The best way is to derive a custom type which
    //    will probably be an overkill here.
    //
    bool trust (co.trust_yes () ||
                co.trust ().find (cert->fingerprint) != co.trust ().end ());

    if (trust)
    {
      if (verb >= 2)
        info << "certificate for repository " << rl.canonical_name () <<
          " authenticated by command line";

      return cert_auth {move (cert), true};
    }

    if (dependent_trust &&
        icasecmp (*dependent_trust, cert->fingerprint) == 0)
    {
      if (verb >= 2)
        info << "certificate for repository " << rl.canonical_name () <<
          " authenticated by dependent trust";

      return cert_auth {move (cert), false};
    }

    (co.trust_no () ? error : warn)
      << "authenticity of the certificate for repository "
      << rl.canonical_name () << " cannot be established";

    if (!co.trust_no () && verb)
    {
      text << "certificate is for " << cert->name << ", \""
           << cert->organization << "\" <" << cert->email << ">";

      text << "certificate SHA256 fingerprint:";
      text << cert->fingerprint;
    }

    if (co.trust_no () || !yn_prompt ("trust this certificate? [y/n]"))
      throw failed ();

    return cert_auth {move (cert), true};
  }

  // Authenticate a certificate with the database. First check if it is
  // already authenticated. If not, authenticate and add to the database.
  //
  static shared_ptr<certificate>
  auth_cert (const common_options& co,
             database& db,
             const optional<string>& pem,
             const repository_location& rl,
             const optional<string>& dependent_trust)
  {
    tracer trace ("auth_cert");
    tracer_guard tg (db, trace);

    fingerprint fp (cert_fingerprint (co, pem, rl));
    shared_ptr<certificate> cert (db.find<certificate> (fp.abbreviated));

    // If the certificate is in the database then it is authenticated by the
    // user. In this case the dependent trust doesn't really matter as the
    // user is more authoritative then the dependent.
    //
    if (cert != nullptr)
    {
      l4 ([&]{trace << "existing cert: " << *cert;});
      verify_cert (*cert, rl);
      return cert;
    }

    // Note that an unsigned certificate use cannot be authenticated by the
    // dependent trust.
    //
    cert_auth ca (pem
                  ? auth_real (co, fp, *pem, rl, dependent_trust)
                  : cert_auth {auth_dummy (co, fp.abbreviated, rl), true});

    cert = move (ca.cert);

    // Persist the certificate only if it is authenticated by the user.
    //
    if (ca.user)
    {
      db.persist (cert);

      // Save the certificate file.
      //
      if (pem)
      {
        path f (db.config_orig / certs_dir / path (cert->id + ".pem"));

        try
        {
          ofdstream ofs (f);
          ofs << *pem;
          ofs.close ();
        }
        catch (const io_error& e)
        {
          fail << "unable to write certificate to " << f << ": " << e;
        }
      }
    }

    return cert;
  }

  shared_ptr<const certificate>
  authenticate_certificate (const common_options& co,
                            const dir_path* conf,
                            database* db,
                            const optional<string>& pem,
                            const repository_location& rl,
                            const optional<string>& dependent_trust)
  {
    tracer trace ("authenticate_certificate");

    if (co.trust_no () && co.trust_yes ())
      fail << "--trust-yes and --trust-no are mutually exclusive";

    shared_ptr<certificate> r;

    if (conf == nullptr)
    {
      assert (db == nullptr);

      // If we have no configuration, go straight to authenticating a new
      // certificate.
      //
      fingerprint fp (cert_fingerprint (co, pem, rl));
      r = pem
        ? auth_real  (co, fp, *pem, rl, dependent_trust).cert
        : auth_dummy (co, fp.abbreviated, rl);
    }
    else if (db != nullptr)
    {
      assert (transaction::has_current ());

      r = auth_cert (co,
                     *db,
                     pem,
                     rl,
                     dependent_trust);
    }
    else
    {
      database db (*conf, trace, false /* pre_attach */);
      transaction t (db);
      r = auth_cert (co, db, pem, rl, dependent_trust);
      t.commit ();
    }

    return r;
  }

  void
  authenticate_repository (const common_options& co,
                           const dir_path* conf,
                           const optional<string>& cert_pem,
                           const certificate& cert,
                           const signature_manifest& sm,
                           const repository_location& rl)
  {
    tracer trace ("authenticate_repository");

    path f;
    auto_rmfile rm;

    // If we have no configuration or the certificate was authenticated by the
    // dependent trust (see auth_cert() function for details), create the
    // temporary certificate PEM file.
    //
    if (conf == nullptr ||
        !exists (f = *conf / certs_dir / path (cert.id + ".pem")))
    {
      assert (cert_pem);

      try
      {
        rm = tmp_file (conf != nullptr ? *conf : empty_dir_path, "cert");
        f = rm.path;

        ofdstream ofs (f);
        ofs << *cert_pem;
        ofs.close ();
      }
      catch (const io_error& e)
      {
        fail << "unable to save certificate to temporary file " << f
             << ": " << e;
      }
      catch (const system_error& e)
      {
        fail << "unable to obtain temporary file: " << e;
      }
    }

    // Make sure the names are either equal or the certificate name is a
    // prefix (at /-boundary) of the repository name. Note that the certificate
    // name can start with a hostname containing a subdomain wildcard, and
    // having one of the following forms/meanings:
    //
    // *.example.com  - matches any single-level subdomain of example.com
    // **.example.com - matches any subdomain of example.com
    // *example.com   - matches example.com and its any single-level subdomain
    // **example.com  - matches example.com and its any subdomain
    //
    // We will compare the leading name parts (the first components) separately
    // from the trailing parts. Note that the leading part will be empty for
    // the name being an absolute POSIX path. Also note that we currently
    // don't support certificate names that are absolute Windows paths.
    //
    // @@ Supporting Windows absolute paths, in particular, will require to
    //    exclude esc_ctrl specifier from the printing certificate info command
    //    (see above) to keep the backslash unescaped. In the openssl
    //    configuration file the repository path backslashes should, on the
    //    contrary, be escaped.
    //
    // Split a name into the leading and trailing parts.
    //
    auto split = [] (const string& name) -> pair<string, string>
    {
      size_t p (name.find ('/'));
      return make_pair (name.substr (0, p),
                        p != string::npos ? name.substr (p + 1) : string ());
    };

    pair<string, string> c (split (cert.name));

    // Strip 'pkg:' prefix.
    //
    pair<string, string> r (split (rl.canonical_name ().substr (4)));

    // Match the repository canonical name leading part.
    //
    bool match (false);
    {
      string& cp (c.first);
      const string& rp (r.first);

      if (cp[0] == '*') // Subdomain wildcard.
      {
        size_t p (1);

        bool any (cp[p] == '*');
        if (any)
          ++p;

        bool self (cp[p] != '.');
        if (!self)
          ++p;

        cp = cp.substr (p); // Strip wildcard prefix.

        const size_t cn (cp.size ());
        const size_t rn (rp.size ());

        // If hostnames are equal, then the repository hostname matches the
        // certificate hostname if self-matching is allowed. Otherwise, it
        // matches being a subdomain of the first level, or any level if
        // allowed.
        //
        if (rp == cp)
          match = self;
        else if (rn > cn && rp.compare (p = rn - cn, cn, cp) == 0 &&
                 rp[p - 1] == '.')
          match = any || rp.find ('.') == p - 1;
      }
      else
        // If the certificate leading part doesn't contain a subdomain
        // wildcard, then the repository leading part must match it exactly.
        //
        match = rp == cp;
    }

    // Match the repository canonical name trailing part. The certificate name
    // trailing part must be equal to it or be its prefix (at /-boundary).
    //
    if (match)
    {
      const string& cp (c.second);
      const string& rp (r.second);
      const size_t cn (cp.size ());
      const size_t rn (rp.size ());

      // Empty path is considered a prefix of any path.
      //
      match = cn == 0 || (rp.compare (0, cn, cp) == 0 &&
                          (rn == cn || (rn > cn && rp[cn] == '/')));
    }

    if (!match)
      fail << "certificate name mismatch for repository "
           << rl.canonical_name () <<
        info << "certificate name is " << cert.name;

    auto auth_failed = [&rl] (const exception* e = nullptr)
    {
      diag_record dr (error);
      dr << "unable to authenticate repository " << rl.canonical_name ();

      if (e != nullptr)
        dr << ": " << *e;
    };

    bool ku (use_openssl_pkeyutl (co));
    const string& cmd (ku ? openssl_pkeyutl_cmd : openssl_rsautl_cmd);

    const path& openssl_path (co.openssl ()[cmd]);
    const strings& openssl_opts (co.openssl_option ()[cmd]);

    try
    {
      openssl os (print_command,
                  path ("-"), fdstream_mode::text, 2,
                  openssl_path, cmd,
                  openssl_opts,
                  ku ? "-verifyrecover" : "-verify",
                  "-certin",
                  "-inkey",
                  f);

      for (const auto& c: sm.signature)
        os.out.put (c); // Sets badbit on failure.

      os.out.close ();

      string s;
      getline (os.in, s);

      bool v (os.in.eof ());
      os.in.close ();

      if (os.wait () && v)
      {
        if (s != sm.sha256sum)
          fail << "packages manifest file signature mismatch for "
               << rl.canonical_name ();

        return; // All good.
      }

      auth_failed ();

      // Fall through.
    }
    catch (const process_error& e)
    {
      error << "unable to execute " << openssl_path << ": " << e;

      // Fall through.
    }
    catch (const io_error& e)
    {
      auth_failed (&e);

      // Fall through.
    }

    throw failed ();
  }

  vector<char>
  sign_repository (const common_options& co,
                   const string& sha256sum,
                   const string& key_name,
                   const string& cert_pem,
                   const dir_path& repository)
  {
    tracer trace ("sign_repository");

    string r (repository.string () +
              dir_path::traits_type::directory_separator);

    // No sense to calculate the fingerprint for the certificate being used
    // just to check the expiration date.
    //
    shared_ptr<certificate> cert (
      parse_cert (co, fingerprint (), cert_pem, r));

    timestamp now (system_clock::now ());

    if (cert->end_date < now)
      fail << "certificate for repository " << r << " has expired";

    using days = chrono::duration<size_t, ratio<3600 * 24>>;

    days left (chrono::duration_cast<days> (cert->end_date - now));
    if (left < days (365))
      warn << "certificate for repository " << r
           << " expires in less than " << left.count () + 1 << " day(s)";

    auto sign_failed = [&r] (const exception* e = nullptr)
    {
      diag_record dr (error);
      dr << "unable to sign repository " << r;

      if (e != nullptr)
        dr << ": " << *e;
    };

    const string& cmd (use_openssl_pkeyutl (co)
                       ? openssl_pkeyutl_cmd
                       : openssl_rsautl_cmd);

    const path& openssl_path (co.openssl ()[cmd]);
    const strings& openssl_opts (co.openssl_option ()[cmd]);

    try
    {
      openssl os (print_command,
                  fdstream_mode::text, path ("-"), 2,
                  openssl_path, cmd,
                  openssl_opts, "-sign", "-inkey", key_name);

      os.out << sha256sum;
      os.out.close ();

      vector<char> signature (os.in.read_binary ());
      os.in.close ();

      if (os.wait ())
        return signature;

      sign_failed ();

      // Fall through.
    }
    catch (const process_error& e)
    {
      error << "unable to execute " << openssl_path << ": " << e;

      // Fall through.
    }
    catch (const io_error& e)
    {
      sign_failed (&e);

      // Fall through.
    }

    throw failed ();
  }

  shared_ptr<certificate>
  parse_certificate (const common_options& co,
                     const string& cert_pem,
                     const repository_location& rl)
  {
    return parse_cert (co,
                       real_fingerprint (co, cert_pem, rl),
                       cert_pem,
                       rl.canonical_name ());
  }
}
