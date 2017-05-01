// file      : bpkg/auth.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <bpkg/auth.hxx>

#include <ratio>
#include <limits>    // numeric_limits
#include <cstring>   // strlen(), strcmp()
#include <iterator>  // ostreambuf_iterator, istreambuf_iterator

#include <butl/sha256>
#include <butl/base64>
#include <butl/process>
#include <butl/fdstream>
#include <butl/filesystem>

#include <bpkg/openssl.hxx>
#include <bpkg/package.hxx>
#include <bpkg/package-odb.hxx>
#include <bpkg/database.hxx>
#include <bpkg/diagnostics.hxx>

using namespace std;
using namespace butl;

namespace bpkg
{
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

    // If this is a remote location then use the canonical name prefix. For
    // a local location this doesn't always work. Consider:
    //
    // .../pkg/1/build2.org/common/hello
    //
    // In this case we will end with an empty canonical name (because of
    // the special pkg/1 treatment). So in case of local locations we will
    // use the location rather than the name prefix.
    //
    if (rl.remote ())
      return repository_location (p.posix_string (), rl).canonical_name ();
    else
      return (rl.path () / p).normalize ().string ();
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

  // Calculate the real repository certificate fingerprint. Return the compact
  // form (no colons, lower case).
  //
  static string
  real_fingerprint (const common_options& co,
                    const string& pem,
                    const repository_location& rl)
  {
    tracer trace ("real_fingerprint");

    try
    {
      process pr (start_openssl (
        co, "x509", {"-sha256", "-noout", "-fingerprint"}, true, true));

      try
      {
        ifdstream is (move (pr.in_ofd), fdstream_mode::skip);
        ofdstream os (move (pr.out_fd));
        os << pem;
        os.close ();

        string s;
        getline (is, s);
        is.close ();

        const size_t n (19);
        if (!(s.size () > n && s.compare (0, n, "SHA256 Fingerprint=") == 0))
          throw io_error ("");

        string fp;

        try
        {
          fp = fingerprint_to_sha256 (string (s, n));
        }
        catch (const invalid_argument&)
        {
          throw io_error ("");
        }

        if (pr.wait ())
          return fp;

        // Fall through.
        //
      }
      catch (const io_error&)
      {
        // Child exit status doesn't matter. Just wait for the process
        // completion and fall through.
        //
        pr.wait (); // Check throw.
      }

      error << "unable to calculate certificate fingerprint for "
            << rl.canonical_name ();

      // Fall through.
    }
    catch (const process_error& e)
    {
      error << "unable to calculate certificate fingerprint for "
            << rl.canonical_name () << ": " << e;

      // Fall through.
    }

    throw failed ();
  }

  // Calculate the repository certificate fingerprint. Return the compact form
  // (no colons, lower case).
  //
  static string
  cert_fingerprint (const common_options& co,
                    const optional<string>& pem,
                    const repository_location& rl)
  {
    return pem
      ? real_fingerprint (co, *pem, rl)
      : sha256 (name_prefix (rl)).string ();
  }

  // Parse the PEM-encoded certificate representation.
  //
  static shared_ptr<certificate>
  parse_cert (const common_options& co,
              const string& fp,
              const string& pem,
              const string& repo)
  {
    tracer trace ("parse_cert");

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
      process pr (start_openssl (
        co,
        "x509",
        {
          "-noout",
          "-subject",
          "-dates",
          "-email",

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
        },
        true,
        true));

      try
      {
        // We unset failbit to provide the detailed error description (which
        // certificate field is missed) on failure.
        //
        ifdstream is (
          move (pr.in_ofd), fdstream_mode::skip, ifdstream::badbit);

        ofdstream os (move (pr.out_fd));

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
        os << pem;
        os.close ();

        auto bad_cert ([](const string& d) {throw invalid_argument (d);});

        auto get = [&is, &trace] (string& s) -> bool
        {
          bool r (getline (is, s));
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

        if (!is || s.compare (0, 10, "notBefore=") != 0)
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
        if (is.peek () != ifdstream::traits_type::eof ())
          bad_cert ("unexpected data");

        is.close ();

        shared_ptr<certificate> cert (
          make_shared<certificate> (
            fp,
            move (name),
            move (org),
            move (email),
            move (not_before),
            move (not_after)));

        if (pr.wait ())
          return cert;

        // Fall through.
        //
      }
      catch (const io_error&)
      {
        // Child exit status doesn't matter. Just wait for the process
        // completion and fall through.
        //
        pr.wait (); // Check throw.
      }
      catch (const invalid_argument& e)
      {
        // If the child exited with an error status, then omit any output
        // parsing diagnostics since we were probably parsing garbage.
        //
        if (pr.wait ())
          fail << "invalid certificate for " << repo << ": " << e << endf;

        // Fall through.
      }

      error << "unable to parse certificate for " << repo;

      // Fall through.
    }
    catch (const process_error& e)
    {
      error << "unable to parse certificate for " << repo << ": " << e;

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

  // Authenticate a real certificate.
  //
  static shared_ptr<certificate>
  auth_real (const common_options& co,
             const string& fp,
             const string& pem,
             const repository_location& rl)
  {
    tracer trace ("auth_real");

    shared_ptr<certificate> cert (
      parse_cert (co, fp, pem, rl.canonical_name ()));

    l4 ([&]{trace << "new cert: " << *cert;});

    verify_cert (*cert, rl);

    string cert_fp (sha256_to_fingerprint (cert->fingerprint));

    // @@ Is there a way to intercept CLI parsing for the specific option of
    // the standard type to validate/convert the value? If there were, we could
    // validate the option value converting fp to sha (internal representation
    // of fp).
    //
    // @@ Not easily/cleanly. The best way is to derive a custom type which
    //    will probably be an overkill here.
    //
    bool trust (co.trust_yes () ||
                co.trust ().find (cert_fp) != co.trust ().end ());

    if (trust)
    {
      if (verb >= 2)
        info << "certificate for repository " << rl.canonical_name () <<
          " authenticated by command line";

      return cert;
    }

    (co.trust_no () ? error : warn)
      << "authenticity of the certificate for repository "
      << rl.canonical_name () << " cannot be established";

    if (!co.trust_no () && verb)
    {
      text << "certificate is for " << cert->name << ", \""
           << cert->organization << "\" <" << cert->email << ">";

      text << "certificate SHA256 fingerprint:";
      text << cert_fp;
    }

    if (co.trust_no () || !yn_prompt ("trust this certificate? [y/n]"))
      throw failed ();

    return cert;
  }

  // Authenticate a certificate with the database. First check if it is
  // already authenticated. If not, authenticate and add to the database.
  //
  static shared_ptr<certificate>
  auth_cert (const common_options& co,
             const dir_path& conf,
             database& db,
             const optional<string>& pem,
             const repository_location& rl)
  {
    tracer trace ("auth_cert");
    tracer_guard tg (db, trace);

    string fp (cert_fingerprint (co, pem, rl));
    shared_ptr<certificate> cert (db.find<certificate> (fp));

    if (cert != nullptr)
    {
      l4 ([&]{trace << "existing cert: " << *cert;});
      verify_cert (*cert, rl);
      return cert;
    }

    cert = pem ? auth_real (co, fp, *pem, rl) : auth_dummy (co, fp, rl);
    db.persist (cert);

    // Save the certificate file.
    //
    if (pem)
    {
      dir_path d (conf / certs_dir);
      if (!dir_exists (d))
        mk (d);

      path f (d / path (fp + ".pem"));

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

    return cert;
  }

  static const dir_path current_dir (".");

  shared_ptr<const certificate>
  authenticate_certificate (const common_options& co,
                            const dir_path* conf,
                            const optional<string>& pem,
                            const repository_location& rl)
  {
    tracer trace ("authenticate_certificate");

    if (co.trust_no () && co.trust_yes ())
      fail << "--trust-yes and --trust-no are mutually exclusive";

    if (conf != nullptr && conf->empty ())
      conf = dir_exists (bpkg_dir) ? &current_dir : nullptr;

    assert (conf == nullptr || !conf->empty ());

    shared_ptr<certificate> r;

    if (conf == nullptr)
    {
      // If we have no configuration, go straight to authenticating a new
      // certificate.
      //
      string fp (cert_fingerprint (co, pem, rl));
      r = pem ? auth_real (co, fp, *pem, rl) : auth_dummy (co, fp, rl);
    }
    else if (transaction::has_current ())
    {
      r = auth_cert (co, *conf, transaction::current ().database (), pem, rl);
    }
    else
    {
      database db (open (*conf, trace));
      transaction t (db.begin ());
      r = auth_cert (co, *conf, db, pem, rl);
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

    if (conf != nullptr && conf->empty ())
      conf = dir_exists (bpkg_dir) ? &current_dir : nullptr;

    assert (conf == nullptr || !conf->empty ());

    path f;
    auto_rmfile rm;

    if (conf == nullptr)
    {
      // If we have no configuration, create the temporary certificate
      // PEM file.
      //
      assert (cert_pem);

      try
      {
        f = path::temp_path ("bpkg");

        ofdstream ofs (f);
        rm = auto_rmfile (f);
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
    else
    {
      f = *conf / certs_dir / path (cert.fingerprint + ".pem");
    }

    const string& c (cert.name);
    const string& r (rl.canonical_name ());
    const size_t cn (c.size ());
    const size_t rn (r.size ());

    // Make sure the names are either equal or the certificate name is a
    // prefix (at /-boundary) of the repository name.
    //
    if (!(r.compare (0, cn, c) == 0 &&
          (rn == cn || (rn > cn && r[cn] == '/'))))
      fail << "certificate name mismatch for repository " << r <<
        info << "certificate name is " << c;

    try
    {
      process pr (start_openssl (
        co, "rsautl",
        {
          "-verify",
          "-certin",
          "-inkey",
          f.string ().c_str ()
        },
        true,
        true));

      try
      {
        ifdstream is (move (pr.in_ofd), fdstream_mode::skip);

        // Write the signature to the openssl process input in the binary mode.
        //
        ofdstream os (move (pr.out_fd), fdstream_mode::binary);

        for (const auto& c: sm.signature)
          os.put (c); // Sets badbit on failure.

        os.close ();

        string s;
        getline (is, s);

        bool v (is.eof ());
        is.close ();

        if (pr.wait () && v)
        {
          if (s != sm.sha256sum)
            fail << "packages manifest file signature mismatch for "
                 << rl.canonical_name ();

          return; // All good.
        }

        // Fall through.
        //
      }
      catch (const io_error&)
      {
        // Child exit status doesn't matter. Just wait for the process
        // completion and fall through.
        //
        pr.wait (); // Check throw.
      }

      error << "unable to authenticate repository " << rl.canonical_name ();

      // Fall through.
    }
    catch (const process_error& e)
    {
      error << "unable to authenticate repository "
            << rl.canonical_name () << ": " << e;

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

    string r (repository.string () + dir_path::traits::directory_separator);

    // No sense to calculate the fingerprint for the certificate being used
    // just to check the expiration date.
    //
    shared_ptr<certificate> cert (parse_cert (co, "", cert_pem, r));

    timestamp now (timestamp::clock::now ());

    if (cert->end_date < now)
      fail << "certificate for repository " << r << " has expired";

    using days = chrono::duration<size_t, ratio<3600 * 24>>;

    days left (chrono::duration_cast<days> (cert->end_date - now));
    if (left < days (60))
      warn << "certificate for repository " << r
           << " expires in less than " << left.count () + 1 << " day(s)";

    try
    {
      process pr (start_openssl (
        co, "rsautl", {"-sign", "-inkey", key_name.c_str ()}, true, true));

      try
      {
        // Read the signature from the openssl process output in the binary
        // mode.
        //
        ifdstream is (
          move (pr.in_ofd), fdstream_mode::binary | fdstream_mode::skip);

        ofdstream os (move (pr.out_fd));
        os << sha256sum;
        os.close ();

        // Additional parentheses required to make compiler to distinguish
        // the variable definition from a function declaration.
        //
        vector<char> signature
          ((istreambuf_iterator<char> (is)), istreambuf_iterator<char> ());

        is.close ();

        if (pr.wait ())
          return signature;

        // Fall through.
        //
      }
      catch (const io_error&)
      {
        // Child exit status doesn't matter. Just wait for the process
        // completion and fall through.
        //
        pr.wait (); // Check throw.
      }

      error << "unable to sign repository " << r;

      // Fall through.
    }
    catch (const process_error& e)
    {
      error << "unable to sign repository " << r << ": " << e;

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
