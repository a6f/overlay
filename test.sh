#!/bin/bash -eux

overlay=${0%/*}/overlay
[[ $overlay = /* ]] || overlay=$PWD/$overlay
test -x $overlay

create_node() {
  case $1 in
    none)     ;;
    dir)      mkdir $2; > $2/$3 ;;
    file)     echo $3 > $2 ;;
    symlink)  ln -s $3 $2 ;;
  esac
}

verify_node() {
  case $1 in
    none)     test ! -e $2 ;;
    dir)      test -d $2; test -f $2/$3 ;;
    file)     test -f $2; test $3 = "$(<$2)" ;;
    symlink)  test -L $2; test $3 = "$(readlink $2)" ;;
  esac
}
export -f verify_node

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' exit
cd $tmpdir

# Test every combination of base and overlay.
for base in none dir file symlink; do
  rm -rf /tmp/node
  create_node $base /tmp/node base
  verify_node $base /tmp/node base
  for top in dir file symlink; do
    rm -rf root
    mkdir -p root/tmp
    create_node $top root/tmp/node top
    verify_node $top root/tmp/node top
    ! bash -c 'verify_node $@' - $top /tmp/node top
    $overlay root bash -c 'verify_node $@' - $top /tmp/node top
  done
done

# Test interaction with some real filesystem entries.
for f1 in '' /x; do
  for f2 in '' /bin/x; do
    for f3 in '' /bin/cp; do
      for f4 in '' /usr/lib/foo/bar/x; do
        rm -rf root
        mkdir root
        for i in $f1 $f2 $f3 $f4; do
          mkdir -p root/"$(dirname $i)"
          echo fnord > root/$i
        done
        $overlay root bash -euxc '
          for i in / /tmp /bin/ls; do
            test -x $i
          done
          for i in $*; do
            test fnord = "$(<$i)"
          done
        ' - $f1 $f2 $f3 $f4
      done
    done
  done
done
