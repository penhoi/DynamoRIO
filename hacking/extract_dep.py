import os, sys

BLD_DIR = ""
BLD_LOG = ""

# build_target is a list []
def do_compiling(DR_ROOT, build_targets):
    assert (len(build_targets) != 0)
    assert (BLD_DIR != "")
    assert( BLD_LOG != "")

    if not os.path.exists(BLD_DIR):
        cmd = "mkdir -p " + BLD_DIR
        os.system(cmd)

    cmd = "cd " + BLD_DIR + " && cmake -DDEBUG=ON .."
    os.system(cmd)

    target = " ".join(build_targets)
    cmd = "cd " + BLD_DIR + " && make -j --trace " + target + " 2>&1 > " + BLD_LOG
    os.system(cmd)


# Read all lines of a logfile
def read_logs(flog):
    lines = []
    with open(flog, "r") as f:
        lines = f.readlines()
    return lines


# Extact paths of all *asm* files
def get_asm_file_path(log_lines):
    paths = []
    for l in log_lines:
        #format cd ... && ...
        if len(l) < 3 or l[0:3] != "cd ":
            continue

        fs = l.split()
        if len(fs) < 2 or fs[2] != "&&":
            continue

        #the last field is a "asm" file
        for f in fs:
            if len(f) < 4 or f[-4::] != ".asm":
                continue
            paths.append(f)
    return paths


# Extact paths of all *cxx* files
def get_cxx_file_path(log_lines):
    paths = []
    for l in log_lines:
        #format cd ... && ...
        if len(l) < 3 or l[0:3] != "cd ":
            continue

        fs = l.split()
        if len(fs) < 2 or fs[2] != "&&":
            continue

        #the last field is a "asm" file
        for f in fs:
            if len(f) < 2 or f[-2:] != ".c":
                continue
            paths.append(f)

    return paths



# Extact paths of all *header* files
def get_header_file_path(log_lines):
    paths = []
    for l in log_lines:
        #format cd ... && ...
        if len(l) < 3 or l[0:3] != "cd ":
            continue

        fs = l.split()
        if len(fs) < 2 or fs[2] != "&&":
            continue

        #the last field is a "asm" file
        for f in fs:
            if len(f) < 2 or f[-2::] != ".h":
                continue
            paths.append(f)

    return paths


# Extact paths of all *header* files
# format: /usr/bin/cmake -Dfile=xxx xxx -P "/home/sgx/project/dynamorio/make/CMake_asm.cmake"
# format: /usr/bin/cmake -D outfile=xxx xxx -P "/home/sgx/project/dynamorio/make/CMake_asm.cmake"
def get_cmake_file_path(log_lines):
    paths = []
    for l in log_lines:
        #format cd ... && ...
        if len(l) < 3 or l[0:3] != "cd ":
            continue

        fs = l.split()
        if len(fs) < 7 or fs[2] != "&&":
            continue

        if len(fs[3]) < 5 or fs[3][-5:] != "cmake":
            continue

        if (len(fs[4]) < 6 or fs[4][:6] != "-Dfile") and (fs[4] != "-D" or len(fs[5]) < 6 or fs[5][:7] != "outfile"):
            continue

        if fs[-2] != "-P":
            continue

        f = fs[-1].strip('"')
        if f not in paths:
            paths.append(f)

    return paths


# Extact paths of all *header* files
def updatetarget_dueto(log_lines):
    asms = []
    cxxs = []
    hdrs = []

    for l in log_lines:
        #format: update target ... due to: ...
        p1 = l.find("update target ")
        if p1 == -1:
            continue
        p2 = l.find("due to: ", p1)
        if p2 == -1:
            continue

        p2 += len("due to: ")
        fs = l[p2:].split()


        for f in fs:
            if len(f) > 4 and f[-4::] == ".asm" and f not in asms:
                asms.append(f)
            elif len(f) > 2 and f[-2::] == ".c" and f not in cxxs:
                cxxs.append(f)
            elif len(f) > 2 and f[-2::] == ".h" and f not in hdrs:
                hdrs.append(f)

    return asms, cxxs, hdrs

# convert all relative-paths to absolute-paths
def to_absolute_path(DR_ROOT, cxxs):
    fullcxxs = []
    for cxx in cxxs:
        if cxx[0] == "/":
            fullcxxs.append(cxx)
        elif cxx[0:3] == "../":
            p = DR_ROOT + cxx[2:]
            fullcxxs.append(p)
        else:
            p = BLD_DIR + "/" + cxx
            fullcxxs.append(p)

    return fullcxxs


# copy all the files listed in *cxxs* into *temp_source*
def copy_files_to_tempsrc(DR_ROOT, build_dir, cxxs):
    NEW_DR_ROOT = os.path.join(DR_ROOT, "tempsrc")
    bld1 = build_dir
    bld2 = "extra"

    for cxx in cxxs:
        assert(cxx.find(DR_ROOT) == 0)

        fname = NEW_DR_ROOT + cxx[len(DR_ROOT):]
        fname = fname.replace(bld1, bld2)
        dname = os.path.dirname(fname)

        # create the directory if it does not exist
        if not os.path.exists(dname):
            cmd = "mkdir -p " + dname
            os.system(cmd)

        # copy the file if it does not exist
        if not os.path.exists(fname):
            cmd = "cp " + cxx + " " + fname
            os.system(cmd)


if __name__ == "__main__":

    if len(sys.argv) < 3:
        print sys.argv[0], "<DR_ROOT_PATH>", "<target1 target2>"
        exit(0)

    DR_ROOT = os.path.realpath(sys.argv[1])
    TARGETS = sys.argv[2:]

    # Initialize global variables
    bld_dir = "build" + "_".join(TARGETS)
    BLD_DIR = os.path.join(DR_ROOT, bld_dir)
    BLD_LOG = os.path.join(DR_ROOT, bld_dir + ".log")

    do_compiling(DR_ROOT, TARGETS)

    logs = read_logs(BLD_LOG)

    # asms = get_asm_file_path(logs)
    # cxxs = get_cxx_file_path(logs)
    # hdrs = get_header_file_path(logs)
    cmakes = get_cmake_file_path(logs)
    fullcmakes = to_absolute_path(DR_ROOT, cmakes)
    copy_files_to_tempsrc(DR_ROOT, bld_dir, fullcmakes)

    # Get a list of all cxx/hdr files
    asms,cxxs,hdrs = updatetarget_dueto(logs)

    fullasms = to_absolute_path(DR_ROOT, asms)
    fullcxxs = to_absolute_path(DR_ROOT, cxxs)
    fullhdrs = to_absolute_path(DR_ROOT, hdrs)

    copy_files_to_tempsrc(DR_ROOT, bld_dir, fullasms)
    copy_files_to_tempsrc(DR_ROOT, bld_dir, fullcxxs)
    copy_files_to_tempsrc(DR_ROOT, bld_dir, fullhdrs)


    #Copy makefiles, only supporting one target
    NEW_DR_ROOT = os.path.join(DR_ROOT, "tempsrc")
    cmd = "cp " + TARGETS[0] + ".mk " + NEW_DR_ROOT
    os.system(cmd)

    #Copy App.c blindly
    cmd = "cp App.c " + NEW_DR_ROOT
    os.system(cmd)

    #Copy buildenv.mk blindly
    cmd = "cp buildenv.mk " + DR_ROOT
    os.system(cmd)
