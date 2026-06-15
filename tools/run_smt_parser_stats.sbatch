#!/bin/bash -l
#
#SBATCH --job-name=QF_LRA
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=16
#SBATCH --mem=128G
#SBATCH --time=48:00:00
#SBATCH --output=/dev/null
#SBATCH --error=/dev/null

set -euo pipefail

#------------------------------------------------------------------------------
# Paths
#------------------------------------------------------------------------------
# Submit this script from the repo lala-parsing
# Ex: sbatch --export=ALL,SMT_ROOT=../jet/benchs/smt/non-incremental/QF_LRA tools/run_smt_parser_stats.sbatch
# SMT_ROOT, LOG_DIR, and OUTPUT_CSV can be overridden with sbatch --export=ALL,NAME=value.
REPO_ROOT="${SLURM_SUBMIT_DIR:-$(pwd)}"

JOB_NAME="${SLURM_JOB_NAME}"
JOB_ID="${SLURM_JOB_ID}"

LOG_DIR="${LOG_DIR:-../jet/benchs/smt_stats/${JOB_NAME}-${JOB_ID}}"
# stdout and stderr logs
OUT_LOG="${LOG_DIR}/std.out"
ERR_LOG="${LOG_DIR}/std.err"
# Output CSV stats
OUTPUT_CSV="${OUTPUT_CSV:-${LOG_DIR}/smt_parser_stats.csv}"
# smt banchmarks
SMT_ROOT="${SMT_ROOT:-../jet/benchs/smt/non-incremental}"
# Parser executable
BUILD_DIR="${REPO_ROOT}/build/smt-parser-stat"
PROBE="${BUILD_DIR}/smt_parser_probe"

JOBS="${SLURM_CPUS_PER_TASK:-16}"

cd "${REPO_ROOT}"
mkdir -p "${LOG_DIR}"

exec >"${OUT_LOG}" 2>"${ERR_LOG}"

echo "Working directory: ${REPO_ROOT}"
echo "SMT root: ${SMT_ROOT}"
echo "Build directory: ${BUILD_DIR}"
echo "Probe executable: ${PROBE}"
echo "Output CSV: ${OUTPUT_CSV}"
echo "Parser jobs: ${JOBS}"
echo "stdout log: ${OUT_LOG}"
echo "stderr log: ${ERR_LOG}"

#------------------------------------------------------------------------------
# HPC modules
#------------------------------------------------------------------------------
module use "/work/projects/software_set/easybuild/${ULHPC_CLUSTER}/2023b/${RESIF_ARCH}/modules/all"
module load devel/CMake
module load devel/Doxygen
module load lib/libxml2
module load lang/Python

#------------------------------------------------------------------------------
# Build parser-only executable
#------------------------------------------------------------------------------
cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" \
  -DGPU=OFF \
  -DLOCAL_DEPS=ON \
  -DCMAKE_BUILD_TYPE=Release \
  -DWITH_NNV=OFF \
  -DWITH_XCSP3PARSER=OFF \
  -DLALA_PARSING_BUILD_EXAMPLES=OFF

cmake --build "${BUILD_DIR}" --target smt_parser_probe

if [ ! -x "${PROBE}" ]; then
  echo "Error: smt_parser_probe was not built or is not executable: ${PROBE}" >&2
  exit 1
fi

if [ ! -d "${SMT_ROOT}" ] && [ ! -f "${SMT_ROOT}" ]; then
  echo "Error: SMT input path does not exist: ${SMT_ROOT}" >&2
  exit 1
fi

#------------------------------------------------------------------------------
# Run stats mode
#------------------------------------------------------------------------------
python3 tools/run_smt_parser.py \
  "${SMT_ROOT}" \
  --probe "${PROBE}" \
  --output "${OUTPUT_CSV}" \
  --jobs "${JOBS}"
