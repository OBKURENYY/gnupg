/* certchain.c - certificate chain validation
 *	Copyright (C) 2001, 2002, 2003 Free Software Foundation, Inc.
 *
 * This file is part of GnuPG.
 *
 * GnuPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * GnuPG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h> 
#include <time.h>
#include <assert.h>

#include "gpgsm.h"
#include <gcrypt.h>
#include <ksba.h>

#include "keydb.h"
#include "i18n.h"

static int
unknown_criticals (KsbaCert cert)
{
  static const char *known[] = {
    "2.5.29.15", /* keyUsage */
    "2.5.29.19", /* basic Constraints */
    "2.5.29.32", /* certificatePolicies */
    NULL
  };
  int rc = 0, i, idx, crit;
  const char *oid;
  KsbaError err;

  for (idx=0; !(err=ksba_cert_get_extension (cert, idx,
                                             &oid, &crit, NULL, NULL));idx++)
    {
      if (!crit)
        continue;
      for (i=0; known[i] && strcmp (known[i],oid); i++)
        ;
      if (!known[i])
        {
          log_error (_("critical certificate extension %s is not supported\n"),
                     oid);
          rc = gpg_error (GPG_ERR_UNSUPPORTED_CERT);
        }
    }
  if (err && err != -1)
    rc = map_ksba_err (err);

  return rc;
}

static int
allowed_ca (KsbaCert cert, int *chainlen)
{
  KsbaError err;
  int flag;

  err = ksba_cert_is_ca (cert, &flag, chainlen);
  if (err)
    return map_ksba_err (err);
  if (!flag)
    {
      log_error (_("issuer certificate is not marked as a CA\n"));
      return gpg_error (GPG_ERR_BAD_CA_CERT);
    }
  return 0;
}


static int
check_cert_policy (KsbaCert cert)
{
  KsbaError err;
  char *policies;
  FILE *fp;
  int any_critical;

  err = ksba_cert_get_cert_policies (cert, &policies);
  if (err == KSBA_No_Data)
    return 0; /* no policy given */
  if (err)
    return map_ksba_err (err);

  /* STRING is a line delimited list of certifiate policies as stored
     in the certificate.  The line itself is colon delimited where the
     first field is the OID of the policy and the second field either
     N or C for normal or critical extension */

  if (opt.verbose > 1)
    log_info ("certificate's policy list: %s\n", policies);

  /* The check is very minimal but won't give false positives */
  any_critical = !!strstr (policies, ":C");

  if (!opt.policy_file)
    { 
      xfree (policies);
      if (any_critical)
        {
          log_error ("critical marked policy without configured policies\n");
          return gpg_error (GPG_ERR_NO_POLICY_MATCH);
        }
      return 0;
    }

  fp = fopen (opt.policy_file, "r");
  if (!fp)
    {
      log_error ("failed to open `%s': %s\n",
                 opt.policy_file, strerror (errno));
      xfree (policies);
      return gpg_error (GPG_ERR_NO_POLICY_MATCH);
    }

  for (;;) 
    {
      int c;
      char *p, line[256];
      char *haystack, *allowed;

      /* read line */
      do
        {
          if (!fgets (line, DIM(line)-1, fp) )
            {
              gpg_error_t tmperr;

              xfree (policies);
              if (feof (fp))
                {
                  fclose (fp);
                  /* with no critical policies this is only a warning */
                  if (!any_critical)
                    {
                      log_info (_("note: certificate policy not allowed\n"));
                      return 0;
                    }
                  log_error (_("certificate policy not allowed\n"));
                  return gpg_error (GPG_ERR_NO_POLICY_MATCH);
                }
              tmperr = gpg_error (gpg_err_code_from_errno (errno));
              fclose (fp);
              return tmperr;
            }
      
          if (!*line || line[strlen(line)-1] != '\n')
            {
              /* eat until end of line */
              while ( (c=getc (fp)) != EOF && c != '\n')
                ;
              fclose (fp);
              xfree (policies);
              return gpg_error (*line? GPG_ERR_LINE_TOO_LONG
                                     : GPG_ERR_INCOMPLETE_LINE);
            }
          
          /* Allow for empty lines and spaces */
          for (p=line; spacep (p); p++)
            ;
        }
      while (!*p || *p == '\n' || *p == '#');
  
      /* parse line */
      for (allowed=line; spacep (allowed); allowed++)
        ;
      p = strpbrk (allowed, " :\n");
      if (!*p || p == allowed)
        {
          fclose (fp);
          xfree (policies);
          return gpg_error (GPG_ERR_CONFIGURATION);
        }
      *p = 0; /* strip the rest of the line */
      /* See whether we find ALLOWED (which is an OID) in POLICIES */
      for (haystack=policies; (p=strstr (haystack, allowed)); haystack = p+1)
        {
          if ( !(p == policies || p[-1] == '\n') )
            continue; /* does not match the begin of a line */
          if (p[strlen (allowed)] != ':')
            continue; /* the length does not match */
          /* Yep - it does match so return okay */
          fclose (fp);
          xfree (policies);
          return 0;
        }
    }
}


static void
find_up_store_certs_cb (void *cb_value, KsbaCert cert)
{
  if (keydb_store_cert (cert, 1, NULL))
    log_error ("error storing issuer certificate as ephemeral\n");
  ++*(int*)cb_value;
}


static int
find_up (KEYDB_HANDLE kh, KsbaCert cert, const char *issuer)
{
  KsbaName authid;
  KsbaSexp authidno;
  int rc = -1;

  if (!ksba_cert_get_auth_key_id (cert, NULL, &authid, &authidno))
    {
      const char *s = ksba_name_enum (authid, 0);
      if (s && *authidno)
        {
          rc = keydb_search_issuer_sn (kh, s, authidno);
          if (rc)
              keydb_search_reset (kh);
          if (rc == -1)
            { /* And try the ephemeral DB. */
              int old = keydb_set_ephemeral (kh, 1);
              if (!old)
                {
                  rc = keydb_search_issuer_sn (kh, s, authidno);
                  if (rc)
                    keydb_search_reset (kh);
                }
              keydb_set_ephemeral (kh, old);
            }
        }
      /* print a note so that the user does not feel too helpless when
         an issuer certificate was found and gpgsm prints BAD
         signature becuase it is not the correct one. */
      if (rc == -1)
        {
          log_info ("issuer certificate (#");
          gpgsm_dump_serial (authidno);
          log_printf ("/");
          gpgsm_dump_string (s);
          log_printf (") not found\n");
        }
      else if (rc)
        log_error ("failed to find authorityKeyIdentifier: rc=%d\n", rc);
      ksba_name_release (authid);
      xfree (authidno);
      /* Fixme: don't know how to do dirmngr lookup with serial+issuer. */
    }
  
  if (rc) /* not found via authorithyKeyIdentifier, try regular issuer name */
      rc = keydb_search_subject (kh, issuer);
  if (rc == -1)
    {
      /* Not found, lets see whether we have one in the ephemeral key DB. */
      int old = keydb_set_ephemeral (kh, 1);
      if (!old)
        {
          keydb_search_reset (kh);
          rc = keydb_search_subject (kh, issuer);
        }
      keydb_set_ephemeral (kh, old);
    }

  if (rc == -1 && opt.auto_issuer_key_retrieve)
    {
      STRLIST names = NULL;
      int count = 0;
      char *pattern;
      const char *s;
      
      if (opt.verbose)
        log_info (_("looking up issuer at external location\n"));
      /* dirmngr is confused about unknown attributes so has a quick
         and ugly hack we locate the CN and use this and the
         following.  Fixme: we should have far better parsing in the
         dirmngr. */
      s = strstr (issuer, "CN=");
      if (!s || s == issuer || s[-1] != ',')
        s = issuer;

      pattern = xtrymalloc (strlen (s)+2);
      if (!pattern)
        return OUT_OF_CORE (errno);
      strcpy (stpcpy (pattern, "/"), s);
      add_to_strlist (&names, pattern);
      xfree (pattern);
      rc = gpgsm_dirmngr_lookup (NULL, names, find_up_store_certs_cb, &count);
      free_strlist (names);
      if (opt.verbose)
        log_info (_("number of issuers matching: %d\n"), count);
      if (rc) 
        {
          log_error ("external key lookup failed: %s\n", gpg_strerror (rc));
          rc = -1;
        }
      else if (!count)
        rc = -1;
      else
        {
          int old;
          /* The issuers are currently stored in the ephemeral key
             DB, so we temporary switch to ephemeral mode. */
          old = keydb_set_ephemeral (kh, 1);
          keydb_search_reset (kh);
          rc = keydb_search_subject (kh, issuer);
          keydb_set_ephemeral (kh, old);
        }
    }
  return rc;
}


/* Return the next certificate up in the chain starting at START.
   Returns -1 when there are no more certificates. */
int
gpgsm_walk_cert_chain (KsbaCert start, KsbaCert *r_next)
{
  int rc = 0; 
  char *issuer = NULL;
  char *subject = NULL;
  KEYDB_HANDLE kh = keydb_new (0);

  *r_next = NULL;
  if (!kh)
    {
      log_error (_("failed to allocated keyDB handle\n"));
      rc = gpg_error (GPG_ERR_GENERAL);
      goto leave;
    }

  issuer = ksba_cert_get_issuer (start, 0);
  subject = ksba_cert_get_subject (start, 0);
  if (!issuer)
    {
      log_error ("no issuer found in certificate\n");
      rc = gpg_error (GPG_ERR_BAD_CERT);
      goto leave;
    }
  if (!subject)
    {
      log_error ("no subject found in certificate\n");
      rc = gpg_error (GPG_ERR_BAD_CERT);
      goto leave;
    }

  if (!strcmp (issuer, subject))
    {
      rc = -1; /* we are at the root */
      goto leave; 
    }

  rc = find_up (kh, start, issuer);
  if (rc)
    {
      /* it is quite common not to have a certificate, so better don't
         print an error here */
      if (rc != -1 && opt.verbose > 1)
        log_error ("failed to find issuer's certificate: rc=%d\n", rc);
      rc = gpg_error (GPG_ERR_MISSING_CERT);
      goto leave;
    }

  rc = keydb_get_cert (kh, r_next);
  if (rc)
    {
      log_error ("failed to get cert: rc=%d\n", rc);
      rc = gpg_error (GPG_ERR_GENERAL);
    }

 leave:
  xfree (issuer);
  xfree (subject);
  keydb_release (kh); 
  return rc;
}


/* Check whether the CERT is a root certificate.  Returns True if this
   is the case. */
int
gpgsm_is_root_cert (KsbaCert cert)
{
  char *issuer;
  char *subject;
  int yes;

  issuer = ksba_cert_get_issuer (cert, 0);
  subject = ksba_cert_get_subject (cert, 0);
  yes = (issuer && subject && !strcmp (issuer, subject));
  xfree (issuer);
  xfree (subject);
  return yes;
}


/* Validate a chain and optionally return the nearest expiration time
   in R_EXPTIME */
int
gpgsm_validate_chain (CTRL ctrl, KsbaCert cert, time_t *r_exptime)
{
  int rc = 0, depth = 0, maxdepth;
  char *issuer = NULL;
  char *subject = NULL;
  KEYDB_HANDLE kh = keydb_new (0);
  KsbaCert subject_cert = NULL, issuer_cert = NULL;
  time_t current_time = gnupg_get_time ();
  time_t exptime = 0;
  int any_expired = 0;
  int any_revoked = 0;
  int any_no_crl = 0;
  int any_crl_too_old = 0;
  int any_no_policy_match = 0;

  if (r_exptime)
    *r_exptime = 0;

  if (opt.no_chain_validation)
    {
      log_info ("WARNING: bypassing certificate chain validation\n");
      return 0;
    }
  
  if (!kh)
    {
      log_error (_("failed to allocated keyDB handle\n"));
      rc = gpg_error (GPG_ERR_GENERAL);
      goto leave;
    }

  if (DBG_X509)
    gpgsm_dump_cert ("subject", cert);

  subject_cert = cert;
  maxdepth = 50;

  for (;;)
    {
      xfree (issuer);
      xfree (subject);
      issuer = ksba_cert_get_issuer (subject_cert, 0);
      subject = ksba_cert_get_subject (subject_cert, 0);

      if (!issuer)
        {
          log_error ("no issuer found in certificate\n");
          rc = gpg_error (GPG_ERR_BAD_CERT);
          goto leave;
        }

      {
        time_t not_before, not_after;

        not_before = ksba_cert_get_validity (subject_cert, 0);
        not_after = ksba_cert_get_validity (subject_cert, 1);
        if (not_before == (time_t)(-1) || not_after == (time_t)(-1))
          {
            log_error ("certificate with invalid validity\n");
            rc = gpg_error (GPG_ERR_BAD_CERT);
            goto leave;
          }

        if (not_after)
          {
            if (!exptime)
              exptime = not_after;
            else if (not_after < exptime)
              exptime = not_after;
          }

        if (not_before && current_time < not_before)
          {
            log_error ("certificate too young; valid from ");
            gpgsm_dump_time (not_before);
            log_printf ("\n");
            rc = gpg_error (GPG_ERR_CERT_TOO_YOUNG);
            goto leave;
          }            
        if (not_after && current_time > not_after)
          {
            log_error ("certificate has expired at ");
            gpgsm_dump_time (not_after);
            log_printf ("\n");
            any_expired = 1;
          }            
      }

      rc = unknown_criticals (subject_cert);
      if (rc)
        goto leave;

      if (!opt.no_policy_check)
        {
          rc = check_cert_policy (subject_cert);
          if (gpg_err_code (rc) == GPG_ERR_NO_POLICY_MATCH)
            {
              any_no_policy_match = 1;
              rc = 1;
            }
          else if (rc)
            goto leave;
        }

      if (!opt.no_crl_check)
        {
          rc = gpgsm_dirmngr_isvalid (subject_cert);
          if (rc)
            {
              switch (rc)
                {
                case GPG_ERR_CERT_REVOKED:
                  log_error (_("the certificate has been revoked\n"));
                  any_revoked = 1;
                  break;
                case GPG_ERR_NO_CRL_KNOWN:
                  log_error (_("no CRL found for certificate\n"));
                  any_no_crl = 1;
                  break;
                case GPG_ERR_CRL_TOO_OLD:
                  log_error (_("the available CRL is too old\n"));
                  log_info (_("please make sure that the "
                              "\"dirmngr\" is properly installed\n"));
                  any_crl_too_old = 1;
                  break;
                default:
                  log_error (_("checking the CRL failed: %s\n"),
                             gpg_strerror (rc));
                  goto leave;
                }
              rc = 0;
            }
        }

      if (subject && !strcmp (issuer, subject))
        {
          if (gpgsm_check_cert_sig (subject_cert, subject_cert) )
            {
              log_error ("selfsigned certificate has a BAD signatures\n");
              rc = gpg_error (depth? GPG_ERR_BAD_CERT_CHAIN
                                   : GPG_ERR_BAD_CERT);
              goto leave;
            }
          rc = allowed_ca (subject_cert, NULL);
          if (rc)
            goto leave;

          rc = gpgsm_agent_istrusted (subject_cert);
          if (!rc)
            ;
          else if (gpg_err_code (rc) == GPG_ERR_NOT_TRUSTED)
            {
              int rc2;

              char *fpr = gpgsm_get_fingerprint_string (subject_cert,
                                                        GCRY_MD_SHA1);
              log_info (_("root certificate is not marked trusted\n"));
              log_info (_("fingerprint=%s\n"), fpr? fpr : "?");
              xfree (fpr);
              rc2 = gpgsm_agent_marktrusted (subject_cert);
              if (!rc2)
                {
                  log_info (_("root certificate has now"
                              " been marked as trusted\n"));
                  rc = 0;
                }
              else 
                {
                  gpgsm_dump_cert ("issuer", subject_cert);
                  log_info ("after checking the fingerprint, you may want "
                            "to enter it manually into "
                            "\"~/.gnupg-test/trustlist.txt\"\n");
                }
            }
          else 
            {
              log_error (_("checking the trust list failed: %s\n"),
                         gpg_strerror (rc));
            }
          
          break;  /* okay, a self-signed certicate is an end-point */
        }
      
      depth++;
      if (depth > maxdepth)
        {
          log_error (_("certificate chain too long\n"));
          rc = gpg_error (GPG_ERR_BAD_CERT_CHAIN);
          goto leave;
        }

      /* find the next cert up the tree */
      keydb_search_reset (kh);
      rc = find_up (kh, subject_cert, issuer);
      if (rc)
        {
          if (rc == -1)
            {
              log_info ("issuer certificate (#/");
              gpgsm_dump_string (issuer);
              log_printf (") not found\n");
            }
          else
            log_error ("failed to find issuer's certificate: rc=%d\n", rc);
          rc = gpg_error (GPG_ERR_MISSING_CERT);
          goto leave;
        }

      ksba_cert_release (issuer_cert); issuer_cert = NULL;
      rc = keydb_get_cert (kh, &issuer_cert);
      if (rc)
        {
          log_error ("failed to get cert: rc=%d\n", rc);
          rc = gpg_error (GPG_ERR_GENERAL);
          goto leave;
        }

      if (DBG_X509)
        {
          log_debug ("got issuer's certificate:\n");
          gpgsm_dump_cert ("issuer", issuer_cert);
        }

      if (gpgsm_check_cert_sig (issuer_cert, subject_cert) )
        {
          log_error ("certificate has a BAD signatures\n");
          rc = gpg_error (GPG_ERR_BAD_CERT_CHAIN);
          goto leave;
        }

      {
        int chainlen;
        rc = allowed_ca (issuer_cert, &chainlen);
        if (rc)
          goto leave;
        if (chainlen >= 0 && (depth - 1) > chainlen)
          {
            log_error (_("certificate chain longer than allowed by CA (%d)\n"),
                       chainlen);
            rc = gpg_error (GPG_ERR_BAD_CERT_CHAIN);
            goto leave;
          }
      }

      rc = gpgsm_cert_use_cert_p (issuer_cert);
      if (rc)
        {
          char numbuf[50];
          sprintf (numbuf, "%d", rc);
          gpgsm_status2 (ctrl, STATUS_ERROR, "certcert.issuer.keyusage",
                         numbuf, NULL);
          rc = 0;
        }

      if (opt.verbose)
        log_info ("certificate is good\n");
      
      keydb_search_reset (kh);
      subject_cert = issuer_cert;
      issuer_cert = NULL;
    }

  if (opt.no_policy_check)
    log_info ("policies not checked due to --disable-policy-checks option\n");
  if (opt.no_crl_check)
    log_info ("CRLs not checked due to --disable-crl-checks option\n");

  if (!rc)
    { /* If we encountered an error somewhere during the checks, set
         the error code to the most critical one */
      if (any_revoked)
        rc = gpg_error (GPG_ERR_CERT_REVOKED);
      else if (any_no_crl)
        rc = gpg_error (GPG_ERR_NO_CRL_KNOWN);
      else if (any_crl_too_old)
        rc = gpg_error (GPG_ERR_CRL_TOO_OLD);
      else if (any_no_policy_match)
        rc = gpg_error (GPG_ERR_NO_POLICY_MATCH);
      else if (any_expired)
        rc = gpg_error (GPG_ERR_CERT_EXPIRED);
    }
  
 leave:
  if (r_exptime)
    *r_exptime = exptime;
  xfree (issuer);
  keydb_release (kh); 
  ksba_cert_release (issuer_cert);
  if (subject_cert != cert)
    ksba_cert_release (subject_cert);
  return rc;
}


/* Check that the given certificate is valid but DO NOT check any
   constraints.  We assume that the issuers certificate is already in
   the DB and that this one is valid; which it should be because it
   has been checked using this function. */
int
gpgsm_basic_cert_check (KsbaCert cert)
{
  int rc = 0;
  char *issuer = NULL;
  char *subject = NULL;
  KEYDB_HANDLE kh = keydb_new (0);
  KsbaCert issuer_cert = NULL;

  if (opt.no_chain_validation)
    {
      log_info ("WARNING: bypassing basic certificate checks\n");
      return 0;
    }

  if (!kh)
    {
      log_error (_("failed to allocated keyDB handle\n"));
      rc = gpg_error (GPG_ERR_GENERAL);
      goto leave;
    }

  issuer = ksba_cert_get_issuer (cert, 0);
  subject = ksba_cert_get_subject (cert, 0);
  if (!issuer)
    {
      log_error ("no issuer found in certificate\n");
      rc = gpg_error (GPG_ERR_BAD_CERT);
      goto leave;
    }

  if (subject && !strcmp (issuer, subject))
    {
      if (gpgsm_check_cert_sig (cert, cert) )
        {
          log_error ("selfsigned certificate has a BAD signatures\n");
          rc = gpg_error (GPG_ERR_BAD_CERT);
          goto leave;
        }
    }
  else
    {
      /* find the next cert up the tree */
      keydb_search_reset (kh);
      rc = find_up (kh, cert, issuer);
      if (rc)
        {
          if (rc == -1)
            {
              log_info ("issuer certificate (#/");
              gpgsm_dump_string (issuer);
              log_printf (") not found\n");
            }
          else
            log_error ("failed to find issuer's certificate: rc=%d\n", rc);
          rc = gpg_error (GPG_ERR_MISSING_CERT);
          goto leave;
        }
      
      ksba_cert_release (issuer_cert); issuer_cert = NULL;
      rc = keydb_get_cert (kh, &issuer_cert);
      if (rc)
        {
          log_error ("failed to get cert: rc=%d\n", rc);
          rc = gpg_error (GPG_ERR_GENERAL);
          goto leave;
        }

      if (gpgsm_check_cert_sig (issuer_cert, cert) )
        {
          log_error ("certificate has a BAD signatures\n");
          rc = gpg_error (GPG_ERR_BAD_CERT);
          goto leave;
        }
      if (opt.verbose)
        log_info ("certificate is good\n");
    }

 leave:
  xfree (issuer);
  keydb_release (kh); 
  ksba_cert_release (issuer_cert);
  return rc;
}
