/* card-util.c - Utility functions for the OpenPGP card.
 *	Copyright (C) 2003 Free Software Foundation, Inc.
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
#include <assert.h>

#if GNUPG_MAJOR_VERSION != 1
#include "gpg.h"
#endif
#include "util.h"
#include "i18n.h"
#include "ttyio.h"
#include "status.h"
#include "options.h"
#include "main.h"
#if GNUPG_MAJOR_VERSION == 1
#include "cardglue.h"
#else
#include "call-agent.h"
#endif

#define CONTROL_D ('D' - 'A' + 1)


/* Change the PIN of a an OpenPGP card.  This is an interactive
   function. */
void
change_pin (int chvno)
{
  struct agent_card_info_s info;
  int rc;

  rc = agent_learn (&info);
  if (rc)
    {
      log_error (_("OpenPGP card not available: %s\n"),
                  gpg_strerror (rc));
      return;
    }
  
  log_info (_("OpenPGP card no. %s detected\n"),
              info.serialno? info.serialno : "[none]");

  agent_release_card_info (&info);

  if (opt.batch)
    {
      log_error (_("sorry, can't do this in batch mode\n"));
      return;
    }

  for (;;)
    {
      char *answer;

      tty_printf ("\n");
      tty_printf ("1 - change PIN\n"
                  "2 - unblock PIN\n"
                  "3 - change Admin PIN\n"
                  "Q - quit\n");
      tty_printf ("\n");

      answer = cpr_get("cardutil.change_pin.menu",_("Your selection? "));
      cpr_kill_prompt();
      if (strlen (answer) != 1)
        continue;

      rc = 0;
      if (*answer == '1')
        {
          rc = agent_scd_change_pin (1);
          if (rc)
            tty_printf ("Error changing the PIN: %s\n", gpg_strerror (rc));
          else
            tty_printf ("PIN changed.\n");
        }
      else if (*answer == '2')
        {
          rc = agent_scd_change_pin (101);
          if (rc)
            tty_printf ("Error unblocking the PIN: %s\n", gpg_strerror (rc));
          else
            tty_printf ("PIN unblocked and new PIN set.\n");
        }
      else if (*answer == '3')
        {
          rc = agent_scd_change_pin (3);
          if (rc)
            tty_printf ("Error changing the PIN: %s\n", gpg_strerror (rc));
          else
            tty_printf ("PIN changed.\n");
        }
      else if (*answer == 'q' || *answer == 'Q')
        {
          break;
        }
    }
}

static const char *
get_manufacturer (unsigned int no)
{
  /* Note:  Make sure that there is no colon or linefeed in the string. */
  switch (no)
    {
    case 0:
    case 0xffff: return "test card";
    case 0x0001: return "PPC Card Systems";
    default: return "unknown";
    }
}


static void
print_sha1_fpr (FILE *fp, const unsigned char *fpr)
{
  int i;

  if (fpr)
    {
      for (i=0; i < 20 ; i+=2, fpr += 2 )
        {
          if (i == 10 )
            tty_fprintf (fp, " ");
          tty_fprintf (fp, " %02X%02X", *fpr, fpr[1]);
        }
    }
  else
    tty_fprintf (fp, " [none]");
  tty_fprintf (fp, "\n");
}


static void
print_sha1_fpr_colon (FILE *fp, const unsigned char *fpr)
{
  int i;

  if (fpr)
    {
      for (i=0; i < 20 ; i++, fpr++)
        fprintf (fp, "%02X", *fpr);
    }
  putc (':', fp);
}


static void
print_name (FILE *fp, const char *text, const char *name)
{
  tty_fprintf (fp, text);


  /* FIXME: tty_printf_utf8_string2 eats everything after and
     including an @ - e.g. when printing an url. */
  if (name && *name)
    {
      if (fp)
        print_utf8_string2 (fp, name, strlen (name), '\n');
      else
        tty_print_utf8_string2 (name, strlen (name), 0);
    }
  else
    tty_fprintf (fp, _("[not set]"));
  tty_fprintf (fp, "\n");
}

static void
print_isoname (FILE *fp, const char *text, const char *tag, const char *name)
{
  if (opt.with_colons)
    fprintf (fp, "%s:", tag);
  else
    tty_fprintf (fp, text);

  if (name && *name)
    {
      char *p, *given, *buf = xstrdup (name);

      given = strstr (buf, "<<");
      for (p=buf; *p; p++)
        if (*p == '<')
          *p = ' ';
      if (given && given[2])
        {
          *given = 0;
          given += 2;
          if (opt.with_colons)
            print_string (fp, given, strlen (given), ':');
          else if (fp)
            print_utf8_string2 (fp, given, strlen (given), '\n');
          else
            tty_print_utf8_string2 (given, strlen (given), 0);

          if (opt.with_colons)
            putc (':', fp);
          else if (*buf)
            tty_fprintf (fp, " ");
        }

      if (opt.with_colons)
        print_string (fp, buf, strlen (buf), ':');
      else if (fp)
        print_utf8_string2 (fp, buf, strlen (buf), '\n');
      else
        tty_print_utf8_string2 (buf, strlen (buf), 0);
      xfree (buf);
    }
  else
    {
      if (opt.with_colons)
        putc (':', fp);
      else
        tty_fprintf (fp, _("[not set]"));
    }

  if (opt.with_colons)
    fputs (":\n", fp);
  else
    tty_fprintf (fp, "\n");
}

/* Return true if the SHA1 fingerprint FPR consists only of zeroes. */
static int
fpr_is_zero (const char *fpr)
{
  int i;

  for (i=0; i < 20 && !fpr[i]; i++)
    ;
  return (i == 20);
}


/* Print all available information about the current card. */
void
card_status (FILE *fp, char *serialno, size_t serialnobuflen)
{
  struct agent_card_info_s info;
  PKT_public_key *pk = xcalloc (1, sizeof *pk);
  int rc;
  unsigned int uval;

  if (serialno && serialnobuflen)
    *serialno = 0;

  rc = agent_learn (&info);
  if (rc)
    {
      if (opt.with_colons)
        fputs ("AID:::\n", fp);
      log_error (_("OpenPGP card not available: %s\n"),
                  gpg_strerror (rc));
      xfree (pk);
      return;
    }

  if (opt.with_colons)
    fprintf (fp, "AID:%s:", info.serialno? info.serialno : "");
  else
    tty_fprintf (fp, "Application ID ...: %s\n",
                 info.serialno? info.serialno : "[none]");
  if (!info.serialno || strncmp (info.serialno, "D27600012401", 12) 
      || strlen (info.serialno) != 32 )
    {
      if (opt.with_colons)
        fputs ("unknown:\n", fp);
      log_info ("not an OpenPGP card\n");
      agent_release_card_info (&info);
      xfree (pk);
      return;
    }

  if (!serialno)
    ;
  else if (strlen (serialno)+1 > serialnobuflen)
    log_error ("serial number longer than expected\n");
  else 
    strcpy (serialno, info.serialno);

  if (opt.with_colons)
    fputs ("openpgp-card:\n", fp);


  if (opt.with_colons)
    {
      fprintf (fp, "version:%.4s:\n", info.serialno+12);
      uval = xtoi_2(info.serialno+16)*256 + xtoi_2 (info.serialno+18);
      fprintf (fp, "vendor:%04x:%s:\n", uval, get_manufacturer (uval));
      fprintf (fp, "serial:%.8s:\n", info.serialno+20);
      
      print_isoname (fp, "Name of cardholder: ", "name", info.disp_name);

      fputs ("lang:", fp);
      if (info.disp_lang)
        print_string (fp, info.disp_lang, strlen (info.disp_lang), ':');
      fputs (":\n", fp);

      fprintf (fp, "sex:%c:\n", (info.disp_sex == 1? 'm':
                                 info.disp_sex == 2? 'f' : 'u'));

      fputs ("url:", fp);
      if (info.pubkey_url)
        print_string (fp, info.pubkey_url, strlen (info.pubkey_url), ':');
      fputs (":\n", fp);

      fputs ("login:", fp);
      if (info.login_data)
        print_string (fp, info.login_data, strlen (info.login_data), ':');
      fputs (":\n", fp);

      fprintf (fp, "forcepin:%d:::\n", !info.chv1_cached);
      fprintf (fp, "maxpinlen:%d:%d:%d:\n",
                   info.chvmaxlen[0], info.chvmaxlen[1], info.chvmaxlen[2]);
      fprintf (fp, "pinretry:%d:%d:%d:\n",
                   info.chvretry[0], info.chvretry[1], info.chvretry[2]);
      fprintf (fp, "sigcount:%lu:::\n", info.sig_counter);

      fputs ("fpr:", fp);
      print_sha1_fpr_colon (fp, info.fpr1valid? info.fpr1:NULL);
      print_sha1_fpr_colon (fp, info.fpr2valid? info.fpr2:NULL);
      print_sha1_fpr_colon (fp, info.fpr3valid? info.fpr3:NULL);
      putc ('\n', fp);

    }
  else 
    {
      tty_fprintf (fp, "Version ..........: %.1s%c.%.1s%c\n",
                   info.serialno[12] == '0'?"":info.serialno+12,
                   info.serialno[13],
                   info.serialno[14] == '0'?"":info.serialno+14,
                   info.serialno[15]);
      tty_fprintf (fp, "Manufacturer .....: %s\n", 
                   get_manufacturer (xtoi_2(info.serialno+16)*256
                                     + xtoi_2 (info.serialno+18)));
      tty_fprintf (fp, "Serial number ....: %.8s\n", info.serialno+20);
      
      print_isoname (fp, "Name of cardholder: ", "name", info.disp_name);
      print_name (fp, "Language prefs ...: ", info.disp_lang);
      tty_fprintf (fp,    "Sex ..............: %s\n",
                   info.disp_sex == 1? _("male"):
                   info.disp_sex == 2? _("female") : _("unspecified"));
      print_name (fp, "URL of public key : ", info.pubkey_url);
      print_name (fp, "Login data .......: ", info.login_data);
      tty_fprintf (fp,    "Signature PIN ....: %s\n",
                   info.chv1_cached? _("not forced"): _("forced"));
      tty_fprintf (fp,    "Max. PIN lengths .: %d %d %d\n",
                   info.chvmaxlen[0], info.chvmaxlen[1], info.chvmaxlen[2]);
      tty_fprintf (fp,    "PIN retry counter : %d %d %d\n",
                   info.chvretry[0], info.chvretry[1], info.chvretry[2]);
      tty_fprintf (fp,    "Signature counter : %lu\n", info.sig_counter);
      tty_fprintf (fp, "Signature key ....:");
      print_sha1_fpr (fp, info.fpr1valid? info.fpr1:NULL);
      tty_fprintf (fp, "Encryption key....:");
      print_sha1_fpr (fp, info.fpr2valid? info.fpr2:NULL);
      tty_fprintf (fp, "Authentication key:");
      print_sha1_fpr (fp, info.fpr3valid? info.fpr3:NULL);
      tty_fprintf (fp, "General key info..: "); 
      if (info.fpr1valid && !get_pubkey_byfprint (pk, info.fpr1, 20))
        print_pubkey_info (fp, pk);
      else
        tty_fprintf (fp, "[none]\n");
    }
      
  free_public_key (pk);
  agent_release_card_info (&info);
}


static char *
get_one_name (const char *prompt1, const char *prompt2)
{
  char *name;
  int i;

  for (;;)
    {
      name = cpr_get (prompt1, prompt2);
      if (!name)
        return NULL;
      trim_spaces (name);
      cpr_kill_prompt ();
      for (i=0; name[i] && name[i] >= ' ' && name[i] <= 126; i++)
        ;

      /* The name must be in Latin-1 and not UTF-8 - lacking the code
         to ensure this we restrict it to ASCII. */
      if (name[i])
        tty_printf (_("Error: Only plain ASCII is currently allowed.\n"));
      else if (strchr (name, '<'))
        tty_printf (_("Error: The \"<\" character may not be used.\n"));
      else if (strstr (name, "  "))
        tty_printf (_("Error: Double spaces are not allowed.\n"));    
      else
        return name;
      xfree (name);
    }
}



static int
change_name (void)
{
  char *surname = NULL, *givenname = NULL;
  char *isoname, *p;
  int rc;

  surname = get_one_name ("keygen.smartcard.surname",
                                    _("Cardholder's surname: "));
  givenname = get_one_name ("keygen.smartcard.givenname",
                                       _("Cardholder's given name: "));
  if (!surname || !givenname || (!*surname && !*givenname))
    {
      xfree (surname);
      xfree (givenname);
      return -1; /*canceled*/
    }

  isoname = xmalloc ( strlen (surname) + 2 + strlen (givenname) + 1);
  strcpy (stpcpy (stpcpy (isoname, surname), "<<"), givenname);
  xfree (surname);
  xfree (givenname);
  for (p=isoname; *p; p++)
    if (*p == ' ')
      *p = '<';

  log_debug ("setting Name to `%s'\n", isoname);
  rc = agent_scd_setattr ("DISP-NAME", isoname, strlen (isoname) );
  if (rc)
    log_error ("error setting Name: %s\n", gpg_strerror (rc));

  xfree (isoname);
  return rc;
}


static int
change_url (void)
{
  char *url;
  int rc;

  url = cpr_get ("cardedit.change_url", _("URL to retrieve public key: "));
  if (!url)
    return -1;
  trim_spaces (url);
  cpr_kill_prompt ();

  rc = agent_scd_setattr ("PUBKEY-URL", url, strlen (url) );
  if (rc)
    log_error ("error setting URL: %s\n", gpg_strerror (rc));
  xfree (url);
  return rc;
}

static int
change_login (void)
{
  char *data;
  int rc;

  data = cpr_get ("cardedit.change_login",
                  _("Login data (account name): "));
  if (!data)
    return -1;
  trim_spaces (data);
  cpr_kill_prompt ();

  rc = agent_scd_setattr ("LOGIN-DATA", data, strlen (data) );
  if (rc)
    log_error ("error setting login data: %s\n", gpg_strerror (rc));
  xfree (data);
  return rc;
}

static int
change_lang (void)
{
  char *data, *p;
  int rc;

  data = cpr_get ("cardedit.change_lang",
                  _("Language preferences: "));
  if (!data)
    return -1;
  trim_spaces (data);
  cpr_kill_prompt ();

  if (strlen (data) > 8 || (strlen (data) & 1))
    {
      tty_printf (_("Error: invalid length of preference string.\n"));
      xfree (data);
      return -1;
    }

  for (p=data; *p && *p >= 'a' && *p <= 'z'; p++)
    ;
  if (*p)
    {
      tty_printf (_("Error: invalid characters in preference string.\n"));
      xfree (data);
      return -1;
    }

  rc = agent_scd_setattr ("DISP-LANG", data, strlen (data) );
  if (rc)
    log_error ("error setting lang: %s\n", gpg_strerror (rc));
  xfree (data);
  return rc;
}


static int
change_sex (void)
{
  char *data;
  const char *str;
  int rc;

  data = cpr_get ("cardedit.change_sex",
                  _("Sex ((M)ale, (F)emale or space): "));
  if (!data)
    return -1;
  trim_spaces (data);
  cpr_kill_prompt ();

  if (!*data)
    str = "9";
  else if ((*data == 'M' || *data == 'm') && !data[1])
    str = "1";
  else if ((*data == 'F' || *data == 'f') && !data[1])
    str = "2";
  else 
    {
      tty_printf (_("Error: invalid response.\n"));
      xfree (data);
      return -1;
    }
     
  rc = agent_scd_setattr ("DISP-SEX", str, 1 );
  if (rc)
    log_error ("error setting sex: %s\n", gpg_strerror (rc));
  xfree (data);
  return rc;
}


static void
toggle_forcesig (void)
{
  struct agent_card_info_s info;
  int rc;
  int newstate;

  memset (&info, 0, sizeof info);
  rc = agent_scd_getattr ("CHV-STATUS", &info);
  if (rc)
    {
      log_error ("error getting current status: %s\n", gpg_strerror (rc));
      return;
    }
  newstate = !info.chv1_cached;
  agent_release_card_info (&info);

  rc = agent_scd_setattr ("CHV-STATUS-1", newstate? "\x01":"", 1);
  if (rc)
    log_error ("error toggling signature PIN flag: %s\n", gpg_strerror (rc));
}


static void
generate_card_keys (void)
{
  struct agent_card_info_s info;
  int rc;
  int forced_chv1;

  memset (&info, 0, sizeof info);
  rc = agent_scd_getattr ("KEY-FPR", &info);
  if (!rc)
    rc = agent_scd_getattr ("SERIALNO", &info);
  if (!rc)
    rc = agent_scd_getattr ("CHV-STATUS", &info);
  if (!rc)
    rc = agent_scd_getattr ("DISP-NAME", &info);
  if (rc)
    {
      log_error ("error getting current key info: %s\n", gpg_strerror (rc));
      return;
    }
  if ( (info.fpr1valid && !fpr_is_zero (info.fpr1))
       || (info.fpr2valid && !fpr_is_zero (info.fpr2))
       || (info.fpr3valid && !fpr_is_zero (info.fpr3)))
    {
      tty_printf ("\n");
      log_info ("NOTE: keys are already stored on the card!\n");
      tty_printf ("\n");
      if ( !cpr_get_answer_is_yes( "cardedit.genkeys.replace_keys",
                                  _("Replace existing keys? ")))
        {
          agent_release_card_info (&info);
          return;
        }
    }
  else if (!info.disp_name || !*info.disp_name)
    {
      tty_printf ("\n");
      tty_printf (_("Please note that the factory settings of the PINs are\n"
                    "   PIN = \"%s\"     Admin PIN = \"%s\"\n"
                    "You should change them using the command --change-pin\n"),
                  "123456", "12345678");
      tty_printf ("\n");
    }

  forced_chv1 = !info.chv1_cached;
  if (forced_chv1)
    { /* Switch of the forced mode so that during key generation we
         don't get bothered with PIN queries for each
         self-signature. */
      rc = agent_scd_setattr ("CHV-STATUS-1", "\x01", 1);
      if (rc)
        {
          log_error ("error clearing forced signature PIN flag: %s\n",
                     gpg_strerror (rc));
          return;
        }
    }
  generate_keypair (NULL, info.serialno);
  agent_release_card_info (&info);
  if (forced_chv1)
    { /* Switch back to forced state. */
      rc = agent_scd_setattr ("CHV-STATUS-1", "", 1);
      if (rc)
        {
          log_error ("error setting forced signature PIN flag: %s\n",
                     gpg_strerror (rc));
          return;
        }
    }
}

/* Menu to edit all user changeable values on an OpenPGP card.  Only
   Key creation is not handled here. */
void
card_edit (STRLIST commands)
{
  enum cmdids {
    cmdNOP = 0,
    cmdQUIT, cmdHELP, cmdLIST, cmdDEBUG,
    cmdNAME, cmdURL, cmdLOGIN, cmdLANG, cmdSEX,
    cmdFORCESIG, cmdGENERATE, cmdPASSWD,
    cmdINVCMD
  };

  static struct {
    const char *name;
    enum cmdids id;
    int requires_pin;
    const char *desc;
  } cmds[] = {
    { N_("quit")  , cmdQUIT  , 0, N_("quit this menu") },
    { N_("q")     , cmdQUIT  , 0, NULL   },
    { N_("help")  , cmdHELP  , 0, N_("show this help") },
    {    "?"      , cmdHELP  , 0, NULL   },
    { N_("list")  , cmdLIST  , 0, N_("list all available data") },
    { N_("l")     , cmdLIST  , 0, NULL   },
    { N_("debug") , cmdDEBUG , 0, NULL },
    { N_("name")  , cmdNAME  , 1, N_("change card holder's name") },
    { N_("url")   , cmdURL   , 1, N_("change URL to retrieve key") },
    { N_("login") , cmdLOGIN , 1, N_("change the login name") },
    { N_("lang")  , cmdLANG  , 1, N_("change the language preferences") },
    { N_("sex")   , cmdSEX   , 1, N_("change card holder's sex") },
    { N_("forcesig"),
                  cmdFORCESIG, 1, N_("toggle the signature force PIN flag") },
    { N_("generate"),
                  cmdGENERATE, 1, N_("generate new keys") },
    { N_("passwd"), cmdPASSWD, 0, N_("menu to change or unblock the PIN") },
    { NULL, cmdINVCMD } 
  };
 
  enum cmdids cmd = cmdNOP;
  int have_commands = !!commands;
  int redisplay = 1;
  char *answer = NULL;
  int did_checkpin = 0;
  char serialnobuf[50];


  if (opt.command_fd != -1)
    ;
  else if (opt.batch && !have_commands)
    {
      log_error(_("can't do that in batchmode\n"));
      goto leave;
    }

  for (;;)
    {
      int arg_number;
      const char *arg_string = "";
      char *p;
      int i;
      int requires_pin;
      
      tty_printf("\n");
      if (redisplay )
        {
          if (opt.with_colons)
            {
              card_status (stdout, serialnobuf, DIM (serialnobuf));
              fflush (stdout);
            }
          else
            {
              card_status (NULL, serialnobuf, DIM (serialnobuf));
              tty_printf("\n");
            }
          redisplay = 0;
	}

      do
        {
          xfree (answer);
          if (have_commands)
            {
              if (commands)
                {
                  answer = xstrdup (commands->d);
                  commands = commands->next;
		}
              else if (opt.batch)
                {
                  answer = xstrdup ("quit");
		}
              else
                have_commands = 0;
	    }

	    if (!have_commands)
              {
		answer = cpr_get_no_help("cardedit.prompt", _("Command> "));
		cpr_kill_prompt();
	    }
	    trim_spaces(answer);
	}
      while( *answer == '#' );

      arg_number = 0; /* Yes, here is the init which egcc complains about */
      requires_pin = 0;
      if (!*answer)
        cmd = cmdLIST; /* Default to the list command */
      else if (*answer == CONTROL_D)
        cmd = cmdQUIT;
      else {
        if ((p=strchr (answer,' ')))
          {
            *p++ = 0;
            trim_spaces (answer);
            trim_spaces (p);
            arg_number = atoi(p);
            arg_string = p;
          }

        for (i=0; cmds[i].name; i++ )
          if (!ascii_strcasecmp (answer, cmds[i].name ))
            break;

        cmd = cmds[i].id;
        requires_pin = cmds[i].requires_pin;
      }
      
      if (requires_pin && !did_checkpin)
        {
          int rc = agent_scd_checkpin (serialnobuf);
          if (rc)
            {
              log_error ("error checking the PIN: %s\n", gpg_strerror (rc));
              continue;
            }
          did_checkpin = 1;
        }

      switch (cmd)
        {
        case cmdHELP:
          for (i=0; cmds[i].name; i++ )
            if (cmds[i].desc)
              tty_printf("%-10s %s\n", cmds[i].name, _(cmds[i].desc) );
          break;

        case cmdLIST:
          redisplay = 1;
          break;

        case cmdNAME:
          change_name ();
          break;

        case cmdURL:
          change_url ();
          break;

        case cmdLOGIN:
          change_login ();
          break;

        case cmdLANG:
          change_lang ();
          break;

        case cmdSEX:
          change_sex ();
          break;

        case cmdFORCESIG:
          toggle_forcesig ();
          break;

        case cmdGENERATE:
          generate_card_keys ();
          break;

        case cmdPASSWD:
          change_pin (0);
          did_checkpin = 0; /* Need to reset it of course. */
          break;

        case cmdQUIT:
          goto leave;

        case cmdNOP:
          break;

        case cmdINVCMD:
        default:
          tty_printf ("\n");
          tty_printf (_("Invalid command  (try \"help\")\n"));
          break;
        } /* End command switch. */
    } /* End of main menu loop. */

 leave:
  xfree (answer);
}
