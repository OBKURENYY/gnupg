#! /bin/sh
# Run this to generate all the initial makefiles, etc.
#
# Copyright (C) 2003 g10 Code GmbH
#
# This file is free software; as a special exception the author gives
# unlimited permission to copy and/or distribute it, with or without
# modifications, as long as this notice is preserved.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY, to the extent permitted by law; without even the
# implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

configure_ac="configure.ac"

cvtver () {
  awk 'NR==1 {split($NF,A,".");X=1000000*A[1]+1000*A[2]+A[3];print X;exit 0}'
}

check_version () {
    if [ $(( `("$1" --version || echo "0") | cvtver` >= $2 )) = 1 ]; then
       return 0
    fi
    echo "**Error**: "\`$1\'" not installed or too old." >&2
    echo '           Version '$3' or newer is required.' >&2
    [ -n "$4" ] && echo '           Note that this is part of '\`$4\''.' >&2
    DIE="yes"
    return 1
}

# Allow to override the default tool names
AUTOCONF=${AUTOCONF_PREFIX}${AUTOCONF:-autoconf}${AUTOCONF_SUFFIX}
AUTOHEADER=${AUTOCONF_PREFIX}${AUTOHEADER:-autoheader}${AUTOCONF_SUFFIX}

AUTOMAKE=${AUTOMAKE_PREFIX}${AUTOMAKE:-automake}${AUTOMAKE_SUFFIX}
ACLOCAL=${AUTOMAKE_PREFIX}${ACLOCAL:-aclocal}${AUTOMAKE_SUFFIX}

GETTEXT=${GETTEXT_PREFIX}${GETTEXT:-gettext}${GETTEXT_SUFFIX}
MSGMERGE=${GETTEXT_PREFIX}${MSGMERGE:-msgmerge}${GETTEXT_SUFFIX}

DIE=no
FORCE=
tmp=`dirname $0`
tsdir=`cd "$tmp"; pwd`
if test x"$1" = x"--force"; then
  FORCE=" --force"
  shift
fi

# Reject unsafe characters in $HOME, $tsdir and cwd.  We consider spaces
# as unsafe because it is too easy to get scripts wrong in this regard.
am_lf='
'
case `pwd` in
  *[\;\\\"\#\$\&\'\`$am_lf\ \	]*)
    echo "unsafe working directory name"; DIE=yes;;
esac
case $tsdir in
  *[\;\\\"\#\$\&\'\`$am_lf\ \	]*)
    echo "unsafe source directory: \`$tsdir'"; DIE=yes;;
esac
case $HOME in
  *[\;\\\"\#\$\&\'\`$am_lf\ \	]*)
    echo "unsafe home directory: \`$HOME'"; DIE=yes;;
esac
if test "$DIE" = "yes"; then
  exit 1
fi

# Begin list of optional variables sourced from ~/.gnupg-autogen.rc
w32_toolprefixes=
w32_extraoptions=
w32ce_toolprefixes=
w32ce_extraoptions=
amd64_toolprefixes=
# End list of optional variables sourced from ~/.gnupg-autogen.rc
# What follows are variables which are sourced but default to
# environment variables or lacking them hardcoded values.
#w32root=
#w32ce_root=
#amd64root=

if [ -f "$HOME/.gnupg-autogen.rc" ]; then
    echo "sourcing extra definitions from $HOME/.gnupg-autogen.rc"
    . "$HOME/.gnupg-autogen.rc"
fi

# Convenience option to use certain configure options for some hosts.
myhost=""
myhostsub=""
case "$1" in
    --build-w32)
        myhost="w32"
        ;;
    --build-w32ce)
        myhost="w32"
        myhostsub="ce"
        ;;
    --build-amd64)
        myhost="amd64"
        ;;
    --build*)
        echo "**Error**: invalid build option $1" >&2
        exit 1
        ;;
    *)
        ;;
esac


# ***** W32 build script *******
# Used to cross-compile for Windows.
if [ "$myhost" = "w32" ]; then
    shift
    if [ ! -f $tsdir/scripts/config.guess ]; then
        echo "$tsdir/scripts/config.guess not found" >&2
        exit 1
    fi
    build=`$tsdir/scripts/config.guess`

    case $myhostsub in
        ce)
          w32root="$w32ce_root"
          [ -z "$w32root" ] && w32root="$HOME/w32ce_root"
          toolprefixes="$w32ce_toolprefixes arm-mingw32ce"
          extraoptions="--enable-dirmngr-auto-start --disable-scdaemon "
          extraoptions="$extraoptions --disable-zip --enable-gpg2-is-gpg"
          extraoptions="$extraoptions $w32ce_extraoptions"
          ;;
        *)
          [ -z "$w32root" ] && w32root="$HOME/w32root"
          toolprefixes="$w32_toolprefixes i586-mingw32msvc"
          toolprefixes="$toolprefixes i386-mingw32msvc mingw32"
          extraoptions="--enable-gpgtar $w32_extraoptions"
          ;;
    esac
    echo "Using $w32root as standard install directory" >&2

    # Locate the cross compiler
    crossbindir=
    for host in $toolprefixes; do
        if ${host}-gcc --version >/dev/null 2>&1 ; then
            crossbindir=/usr/${host}/bin
            conf_CC="CC=${host}-gcc"
            break;
        fi
    done
    if [ -z "$crossbindir" ]; then
        echo "Cross compiler kit not installed" >&2
        if [ -z "$sub" ]; then
          echo "Under Debian GNU/Linux, you may install it using" >&2
          echo "  apt-get install mingw32 mingw32-runtime mingw32-binutils" >&2
        fi
        echo "Stop." >&2
        exit 1
    fi

    if [ -f "$tsdir/config.log" ]; then
        if ! head $tsdir/config.log | grep "$host" >/dev/null; then
            echo "Please run a 'make distclean' first" >&2
            exit 1
        fi
    fi

    $tsdir/configure --enable-maintainer-mode --prefix=${w32root}  \
             --host=${host} --build=${build} \
             --with-gpg-error-prefix=${w32root} \
	     --with-ksba-prefix=${w32root} \
	     --with-libgcrypt-prefix=${w32root} \
	     --with-libassuan-prefix=${w32root} \
	     --with-zlib=${w32root} \
	     --with-regex=${w32root} \
             --with-pth-prefix=${w32root} \
             --with-adns=${w32root} \
             ${extraoptions} --disable-g13 "$@"
    rc=$?
    exit $rc
fi
# ***** end W32 build script *******

# ***** AMD64 cross build script *******
# Used to cross-compile for AMD64 (for testing)
if [ "$myhost" = "amd64" ]; then
    shift
    if [ ! -f $tsdir/scripts/config.guess ]; then
        echo "$tsdir/scripts/config.guess not found" >&2
        exit 1
    fi
    build=`$tsdir/scripts/config.guess`

    [ -z "$amd64root" ] && amd64root="$HOME/amd64root"
    echo "Using $amd64root as standard install directory" >&2

    toolprefixes="$amd64_toolprefixes x86_64-linux-gnu amd64-linux-gnu"

    # Locate the cross compiler
    crossbindir=
    for host in $toolprefixes ; do
        if ${host}-gcc --version >/dev/null 2>&1 ; then
            crossbindir=/usr/${host}/bin
            conf_CC="CC=${host}-gcc"
            break;
        fi
    done
    if [ -z "$crossbindir" ]; then
        echo "Cross compiler kit not installed" >&2
        echo "Stop." >&2
        exit 1
    fi

    if [ -f "$tsdir/config.log" ]; then
        if ! head $tsdir/config.log | grep "$host" >/dev/null; then
            echo "Please run a 'make distclean' first" >&2
            exit 1
        fi
    fi

    $tsdir/configure --enable-maintainer-mode --prefix=${amd64root}  \
             --host=${host} --build=${build} \
             --with-gpg-error-prefix=${amd64root} \
	     --with-ksba-prefix=${amd64root} \
	     --with-libgcrypt-prefix=${amd64root} \
	     --with-libassuan-prefix=${amd64root} \
	     --with-zlib=/usr/x86_64-linux-gnu/usr \
             --with-pth-prefix=/usr/x86_64-linux-gnu/usr
    rc=$?
    exit $rc
fi
# ***** end AMD64 cross build script *******


# Grep the required versions from configure.ac
autoconf_vers=`sed -n '/^AC_PREREQ(/ {
s/^.*(\(.*\))/\1/p
q
}' ${configure_ac}`
autoconf_vers_num=`echo "$autoconf_vers" | cvtver`

automake_vers=`sed -n '/^min_automake_version=/ {
s/^.*="\(.*\)"/\1/p
q
}' ${configure_ac}`
automake_vers_num=`echo "$automake_vers" | cvtver`

gettext_vers=`sed -n '/^AM_GNU_GETTEXT_VERSION(/ {
s/^.*\[\(.*\)])/\1/p
q
}' ${configure_ac}`
gettext_vers_num=`echo "$gettext_vers" | cvtver`


if [ -z "$autoconf_vers" -o -z "$automake_vers" -o -z "$gettext_vers" ]
then
  echo "**Error**: version information not found in "\`${configure_ac}\'"." >&2
  exit 1
fi


if check_version $AUTOCONF $autoconf_vers_num $autoconf_vers ; then
    check_version $AUTOHEADER $autoconf_vers_num $autoconf_vers autoconf
fi
if check_version $AUTOMAKE $automake_vers_num $automake_vers; then
  check_version $ACLOCAL $automake_vers_num $autoconf_vers automake
fi
if check_version $GETTEXT $gettext_vers_num $gettext_vers; then
  check_version $MSGMERGE $gettext_vers_num $gettext_vers gettext
fi

if test "$DIE" = "yes"; then
    cat <<EOF

Note that you may use alternative versions of the tools by setting
the corresponding environment variables; see README.SVN for details.

EOF
    exit 1
fi

# Check the git setup.
if [ -d .git ]; then
  if [ -f .git/hooks/pre-commit.sample -a ! -f .git/hooks/pre-commit ] ; then
    cat <<EOF >&2
*** Activating trailing whitespace git pre-commit hook. ***
    For more information see this thread:
      http://mail.gnome.org/archives/desktop-devel-list/2009-May/msg00084html
    To deactivate this pre-commit hook again move .git/hooks/pre-commit
    and .git/hooks/pre-commit.sample out of the way.
EOF
      cp -av .git/hooks/pre-commit.sample .git/hooks/pre-commit
      chmod +x  .git/hooks/pre-commit
  fi
  tmp=$(git config --get filter.cleanpo.clean)
  if [ "$tmp" != "awk '/^\"POT-Creation-Date:/&&!s{s=1;next};!/^#: /{print}'" ]
  then
    echo "*** Adding GIT filter.cleanpo.clean configuration." >&2
    git config --add filter.cleanpo.clean \
        "awk '/^\"POT-Creation-Date:/&&!s{s=1;next};!/^#: /{print}'"
  fi
  if [ -f scripts/git-hooks/commit-msg -a ! -f .git/hooks/commit-msg ] ; then
    cat <<EOF >&2
*** Activating commit log message check hook. ***
EOF
      cp -av scripts/git-hooks/commit-msg .git/hooks/commit-msg
      chmod +x  .git/hooks/commit-msg
  fi
fi

echo "Running aclocal -I m4 -I gl/m4 ${ACLOCAL_FLAGS:+$ACLOCAL_FLAGS }..."
$ACLOCAL -I m4 -I gl/m4 $ACLOCAL_FLAGS
echo "Running autoheader..."
$AUTOHEADER
echo "Running automake --gnu ..."
$AUTOMAKE --gnu;
echo "Running autoconf${FORCE} ..."
$AUTOCONF${FORCE}

echo "You may now run:
  ./configure --sysconfdir=/etc --enable-maintainer-mode --enable-symcryptrun --enable-mailto --enable-gpgtar && make
"
