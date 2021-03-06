
/******************************************************************************
 *
 *  This file is part of canu, a software program that assembles whole-genome
 *  sequencing reads into contigs.
 *
 *  This software is based on:
 *    'Celera Assembler' (http://wgs-assembler.sourceforge.net)
 *    the 'kmer package' (http://kmer.sourceforge.net)
 *  both originally distributed by Applera Corporation under the GNU General
 *  Public License, version 2.
 *
 *  Canu branched from Celera Assembler at its revision 4587.
 *  Canu branched from the kmer project at its revision 1994.
 *
 *  Modifications by:
 *
 *    Brian P. Walenz beginning on 2018-JUL-21
 *      are a 'United States Government Work', and
 *      are released in the public domain
 *
 *  File 'README.licenses' in the root directory of this distribution contains
 *  full conditions and disclaimers for each license.
 */

#include "meryl.H"


//  Yuck.
char  kmerString[256];



void
merylOperation::findMinCount(void) {
  _count = _actCount[0];
  for (uint32 ii=1; ii<_actLen; ii++)
    if (_actCount[ii] < _count)
      _count = _actCount[ii];
}



void
merylOperation::findMaxCount(void) {
  _count = _actCount[0];
  for (uint32 ii=1; ii<_actLen; ii++)
    if (_count < _actCount[ii])
      _count = _actCount[ii];
}



void
merylOperation::findSumCount(void) {
  _count = 0;
  for (uint32 ii=0; ii<_actLen; ii++)
    _count += _actCount[ii];
}



void
merylOperation::initializeThreshold(void) {

  //  If no thresholds to set, nothing to do.

  if ((_fracDist == DBL_MAX) &&
      (_wordFreq == DBL_MAX))
    return;

  //  The problem with using more than one database is that the number of
  //  distinct kmers is not known.

  if (_inputs.size() != 1) {
    fprintf(stderr, "ERROR: operation most-frequent can work with only one meryl database.\n");
    exit(1);
  }

  //  Later, we could allow streaming operations, and construction of statistics on
  //  the fly.

  bool    allDatabase = true;

  for (uint32 ii=0; ii<_inputs.size(); ii++) {
    if (_inputs[ii]->isFromDatabase() == false) {
      fprintf(stderr, "ERROR: input '%s' to operation most-frequent is not a meryl database.\n",
              _inputs[ii]->_name);
      allDatabase = false;
    }
  }

  if (allDatabase == false)
    exit(1);

  for (uint32 ii=0; ii<_inputs.size(); ii++)
    _inputs[ii]->_stream->loadStatistics();

  kmerCountStatistics  *stats = _inputs[0]->_stream->stats();


#warning "need to decide whic direction we're going?"

  if (_fracDist < DBL_MAX) {
    uint64  nKmers       = 0;
    uint64  nKmersTarget = _fracDist * stats->numDistinct();

    for (uint32 ii=0; ii<stats->numFrequencies(); ii++) {
      nKmers += stats->numKmersAtFrequency(ii);

      if (nKmers >= nKmersTarget) {
        _threshold = ii;
        break;
      }
    }

    fprintf(stderr, "For fraction-distinct %f, found threshold %lu\n", _fracDist, _threshold);
  }

#warning "rounding issues in word-frequency!"

  if (_wordFreq < DBL_MAX) {
    _threshold = _wordFreq * stats->numTotal();

    fprintf(stderr, "For word-frequency %f, found threshold %lu\n", _wordFreq, _threshold);
  }

  //  Cleanup.

  for (uint32 ii=0; ii<_inputs.size(); ii++)
    _inputs[ii]->_stream->dropStatistics();
}



bool
merylOperation::initialize(void) {
  bool  proceed = true;

  //fprintf(stderr, "INITIALIZE\n");

  //  Initialize all the inputs this operation might have.

  for (uint32 ii=0; ii<_inputs.size(); ii++)
    _inputs[ii]->initialize();

  //  Decide if we're processing a multi-set.

  for (uint32 ii=0; ii<_inputs.size(); ii++)
    _isMultiSet |= _inputs[ii]->isMultiSet();

  //  Set up the output for the specific kmer database file we're processing.
  //  Note that if this _was_ a counting operation, nextMer_doCounting() just
  //  above will have already created an output for the count, written the
  //  data, and removed the _output pointer.

  if (_output) {
    _output->initialize(0, isMultiSet());
    _writer = _output->getStreamWriter(_fileNumber);
  }

  //  The threshold operations need to decide on a threshold based on the histogram.

  initializeThreshold();

  //  If configuring, or if the operation is pass-through with no output,
  //  don't stream the mers.  This only matters for the root node; the return
  //  value for all other nodes is ignored (those are called above, when
  //  initializing the inputs to this node).

  if (_onlyConfig == true)
    proceed = false;

  if ((_operation == opPassThrough) &&   //  'meryl print DATABASE' uses opPassThrough.
      (_printer   == NULL))              //  but has _printer set.
    proceed = false;                     //  Counting operations do not set _printer.

  return(proceed);
}



//  Perform the counting operation, then close the output.
//
void
merylOperation::doCounting(void) {

  for (uint32 ii=0; ii<_inputs.size(); ii++)
    _inputs[ii]->initialize();

  bool    doSimple  = false;
  uint32  wPrefix   = 0;
  uint64  nPrefix   = 0;
  uint32  wData     = 0;
  uint64  wDataMask = 0;

  configureCounting(_maxMemory,
                    doSimple,
                    wPrefix,
                    nPrefix,
                    wData,
                    wDataMask);

  //if (_operation == opCountSimple)
  //  doSimple = true;

  if (_kmer.merSize() <= 16)
    doSimple = true;

  if (_maxMemory < (uint64)10 * 1024 * 1024 * 1024)
    doSimple = false;

  omp_set_num_threads(_maxThreads);

  if (doSimple)
    countSimple();
  else
    count(wPrefix, nPrefix, wData, wDataMask);

  clearInputs();

  delete _output;
  _output = NULL;
}



//  Convert the presumed counting operation into a pass-through operation.
//  The merylOpStack (meryl.C) will assign inputs/outputs only to the
//  first file, and that is handled in doCounting() above.
//
//  All we need to do here is reset the operation and add an input to the
//  freshly constructed meryl database.
//
//  If there is no inputName, we're just attempting to configure for Canu.
//
void
merylOperation::convertToPassThrough(char *inputName) {

  //clearInputs();
  //clearOutput();

  if (_verbosity >= sayConstruction)
    fprintf(stderr, "merylOp::nextMer()-- CONVERTING '%s' to '%s'.\n",
            toString(opCount), toString(opPassThrough));

  _operation = opPassThrough;

  if ((inputName != NULL) && (inputName[0] != 0) && (_onlyConfig == false))
    addInput(new kmerCountFileReader(inputName));
}



//  Build a list of the inputs that have the smallest kmer, saving their
//  counts in _actCount, and the input that it is from in _actIndex.
bool
merylOperation::nextMer_findSmallestNormal(void) {

  _actLen = 0;                                       //  Reset to nothing on the list.

  for (uint32 ii=0; ii<_inputs.size(); ii++) {
    if (_inputs[ii]->_valid == false)
      continue;

    if ((_actLen == 0) ||                            //  If we have no active kmer, or the input kmer is
        (_inputs[ii]->_kmer < _kmer)) {              //  smaller than the one we have, reset the list.
      _kmer        = _inputs[ii]->_kmer;
      _actCount[0] = _inputs[ii]->_count;
      _actIndex[0] = ii;
      _actLen      = 1;

      if (_verbosity >= sayDetails)
        fprintf(stderr, "merylOp::nextMer()-- Active kmer %s from input %s. reset\n", _kmer.toString(kmerString), _inputs[ii]->_name);
    }

    else if (_inputs[ii]->_kmer == _kmer) {          //  Otherwise, if the input kmer is the one we
      _actCount[_actLen] = _inputs[ii]->_count;      //  have, save the count and input to the lists.
      _actIndex[_actLen] = ii;
      _actLen++;

      if (_verbosity >= sayDetails)
        fprintf(stderr, "merylOp::nextMer()-- Active kmer %s from input %s\n", _kmer.toString(kmerString), _inputs[ii]->_name);
    }

    else {                                           //  Otherwise, the input kmer comes after the
    }                                                //  one we're examining, ignore it.
  }
}



//  For multi-set operation, the list we build must have exactly one item in it.

//  THIS IS WRONG, it needs to build a list with all the stuff with the same kmer AND value.
//  It's up to the operation to decide what to do.
//  So we probably need a new operation 'merge' or something.

//  The action depends on the operations.
//    Intersect -- treat non-multiset as wildcard; add to list with same value
//    Union     -- treat non-multiset as multiset

bool
merylOperation::nextMer_findSmallestMultiSet(void) {

  _actLen = 0;

  //
  //  A first pass to handle the inputs that are multi-sets.
  //    If a union operation, save only the first lowest kmer/value pair.
  //    Otherwise, add it to the list if it is the same or lower.
  //

  for (uint32 ii=0; ii<_inputs.size(); ii++) {
    if ((_inputs[ii]->_valid == false) ||
        (_inputs[ii]->isMultiSet() == false))
      continue;

    if ((_operation == opUnion) ||
        (_operation == opUnionMin) ||
        (_operation == opUnionMax) ||
        (_operation == opUnionSum)) {
      if (((_actLen == 0)) ||
          ((_inputs[ii]->_kmer  < _kmer)) ||
          ((_inputs[ii]->_kmer == _kmer) && (_inputs[ii]->_count < _actCount[0]))) {
        _kmer        = _inputs[ii]->_kmer;
        _actCount[0] = _inputs[ii]->_count;
        _actIndex[0] = ii;
        _actLen      = 1;

        if (_verbosity >= sayDetails)
          fprintf(stderr, "merylOp::nextMer()-- Active kmer %s from input %s. reset\n", _kmer.toString(kmerString), _inputs[ii]->_name);
      }
    }

    else if (((_actLen == 0)) ||
             ((_inputs[ii]->_kmer  < _kmer)) ||
             ((_inputs[ii]->_kmer == _kmer) && (_inputs[ii]->_count < _actCount[0]))) {
      _kmer              = _inputs[ii]->_kmer;
      _actCount[_actLen] = _inputs[ii]->_count;
      _actIndex[_actLen] = ii;
      _actLen++;
    }
  }

  //
  //  A second pass to handle the inputs that are not multi-sets.
  //  - If a union operation, reset the list if anything is strictly smaller.
  //  - Otherwise, treat as a wildcard and add it to the list if the kmer is equal, resetting the value.
  //
  //    In both cases, add if the list is empty.
  //    - For union:      we want to add these kmers/values to the output.
  //    - For intersect:  the intersect logic will reject it, and we'll then load a new kmer for this input.
  //    - For difference: if this is not the first input, it works as expected, no output
  //                      if this is     the first input, it works as expected, the kmer is output (with original value).
  //

  for (uint32 ii=0; ii<_inputs.size(); ii++) {
    if ((_inputs[ii]->_valid == false) ||
        (_inputs[ii]->isMultiSet() == false))
      continue;

    if ((_operation == opUnion) ||
        (_operation == opUnionMin) ||
        (_operation == opUnionMax) ||
        (_operation == opUnionSum)) {
      if (((_actLen == 0)) ||
          ((_inputs[ii]->_kmer  < _kmer)) ||
          ((_inputs[ii]->_kmer == _kmer) && (_inputs[ii]->_count < _actCount[0]))) {
        _kmer        = _inputs[ii]->_kmer;
        _actCount[0] = _inputs[ii]->_count;
        _actIndex[0] = ii;
        _actLen      = 1;

        if (_verbosity >= sayDetails)
          fprintf(stderr, "merylOp::nextMer()-- Active kmer %s from input %s. reset\n", _kmer.toString(kmerString), _inputs[ii]->_name);
      }
    }

    else if (((_actLen == 0)) ||
             ((_inputs[ii]->_kmer  < _kmer)) ||
             ((_inputs[ii]->_kmer == _kmer) && (_inputs[ii]->_count < _actCount[0]))) {
      _kmer              = _inputs[ii]->_kmer;
      _actCount[_actLen] = _inputs[ii]->_count;
      _actIndex[_actLen] = ii;
      _actLen++;
    }
  }
}



//  If no active kmers, we're done.  Several bits of housekeeping need to be done:
//   - Histogram operations need to finish up and report the histogram now.
//     Alternatively, it could be done in the destructor.
//   - Any outputs need to call finishIteration() to rename and/or merge
//     their intermediate outputs.
bool
merylOperation::nextMer_finish(void) {

  if (_verbosity >= sayDetails) {
    fprintf(stderr, "merylOp::nextMer()-- No inputs found, all done here.\n");
    fprintf(stderr, "\n");
  }

  _valid = false;

  if (_operation == opHistogram)
    reportHistogram();

  if (_operation == opStatistics)
    reportStatistics();

  delete _writer;
  _writer = NULL;

  return(false);
}



bool
merylOperation::nextMer(void) {

 nextMerAgain:

  //  Get some logging out of the way.

  if (_verbosity >= sayDetails) {
    fprintf(stderr, "\n");
    fprintf(stderr, "merylOp::nextMer()-- STARTING for operation %s\n",
            toString(_operation));

    if (_verbosity >= sayEverything)
      for (uint32 ii=0; ii<_inputs.size(); ii++)
        fprintf(stderr, "merylOp::nextMer()--   CURRENT STATE: input %s kmer %s count " F_U64 " %s\n",
                _inputs[ii]->_name,
                _inputs[ii]->_kmer.toString(kmerString),
                _inputs[ii]->_count,
                _inputs[ii]->_valid ? "valid" : "INVALID");
  }

  //  Grab the next mer for every input that was active in the last iteration.
  //  (on the first call, all inputs were 'active' last time)

  for (uint32 ii=0; ii<_actLen; ii++) {
    if (_verbosity >= sayDetails)
      fprintf(stderr, "merylOp::nextMer()-- CALL NEXTMER on input actIndex " F_U32 "\n", _actIndex[ii]);
    _inputs[_actIndex[ii]]->nextMer();
  }

  //  Build a list of the inputs that have the smallest kmer, saving their
  //  counts in _actCount, and the input that it is from in _actIndex.

  if (isMultiSet() == false)
    nextMer_findSmallestNormal();
  else
    nextMer_findSmallestMultiSet();

  //  If no active kmers, we're done.

  if (_actLen == 0)
    return(nextMer_finish());

  //  Otherwise, active kmers!  Figure out what the count should be.

  if (_verbosity >= sayDetails)
    fprintf(stderr, "merylOp::nextMer()-- op %s activeLen " F_U32 " kmer %s\n", toString(_operation), _actLen, _kmer.toString(kmerString));

  //  If math-subtract gets implemented, use negative-zero to mean "don't output" and positive-zero
  //  to mean zero.  For now, count=0 means don't output.

  //  Set the count to zero, meaning "don't output the kmer".  Intersect depends on this,
  //  skipping most of it's work if all files don't have the kmer.
  _count = 0;

  switch (_operation) {
    case opCount:
    case opCountForward:
    case opCountReverse:
      fprintf(stderr, "ERROR: got %s, but shouldn't have.\n", toString(_operation));
      assert(0);
      break;

    case opPassThrough:                     //  Result of counting kmers.  Guaranteed to have
      _count = _actCount[0];                //  exactly one input file.  Also the operation that
      break;                                //  'print' of a database has.

    case opLessThan:
      _count = (_actCount[0]  < _threshold) ? _actCount[0] : 0;
      break;

    case opGreaterThan:
      _count = (_actCount[0]  > _threshold) ? _actCount[0] : 0;
      break;

    case opAtLeast:
      _count = (_actCount[0] >= _threshold) ? _actCount[0] : 0;
      break;

    case opAtMost:
      _count = (_actCount[0] <= _threshold) ? _actCount[0] : 0;
      break;

    case opEqualTo:
      _count = (_actCount[0] == _threshold) ? _actCount[0] : 0;
      break;

    case opNotEqualTo:
      _count = (_actCount[0] != _threshold) ? _actCount[0] : 0;
      break;

    case opIncrease:
      if (UINT64_MAX - _actCount[0] < _mathConstant)
        _count = UINT64_MAX;    //  OVERFLOW!
      else
        _count = _actCount[0] + _mathConstant;
      break;

    case opDecrease:
      if (_actCount[0] < _mathConstant)
        _count = 0;             //  UNDERFLOW!
      else
        _count = _actCount[0] - _mathConstant;
      break;

    case opMultiply:
      if (UINT64_MAX / _actCount[0] < _mathConstant)
        _count = UINT64_MAX;    //  OVERFLOW!
      else
        _count = _actCount[0] * _mathConstant;
      break;

    case opDivide:
      if (_mathConstant == 0)
        _count = 0;             //  DIVIDE BY ZERO!
      else
        _count = _actCount[0] / _mathConstant;
      break;

    case opModulo:
      if (_mathConstant == 0)
        _count = 0;             //  DIVIDE BY ZERO!
      else
        _count = _actCount[0] % _mathConstant;
      break;

    case opUnion:                           //  Union
      _count = _actLen;
      break;

    case opUnionMin:                        //  Union, retain smallest count
      findMinCount();
      break;

    case opUnionMax:                        //  Union, retain largest count
      findMaxCount();
      break;

    case opUnionSum:                        //  Union, sum all counts
      findSumCount();
      break;

    case opIntersect:                       //  Intersect
      if (_actLen == _inputs.size())
        _count = _actCount[0];
      break;

    case opIntersectMin:                    //  Intersect, retain smallest count
      if (_actLen == _inputs.size())
        findMinCount();
      break;

    case opIntersectMax:                    //  Intersect, retain largest count
      if (_actLen == _inputs.size())
        findMaxCount();
      break;

    case opIntersectSum:                    //  Intersect, sum all counts
      if (_actLen == _inputs.size())
        findSumCount();
      break;

    case opDifference:
      if ((_actLen == 1) && (_actIndex[0] == 0))
        _count = _actCount[0];
      break;

    case opSymmetricDifference:
      if (_actLen == 1)
        _count = _actCount[0];
      break;

    case opCompare:
      if       (_actLen == 1) {
        char  str[33];

        fprintf(stdout, "kmer %s only in input %u\n",
                _kmer.toString(str), _actIndex[0]);
      }
      else if ((_actLen == 2) && (_actCount[0] != _actCount[1])) {
        char  str[33];

        fprintf(stdout, "kmer %s has value %lu in input 1 != value %lu in input 2\n",
                _kmer.toString(str), _actCount[0], _actCount[1]);
      }
      else {
      }

    case opHistogram:
      break;

    case opStatistics:
      break;

    case opNothing:
      break;
  }

  //  If the count is zero, skip this kmer and get another one.

  if (_count == 0)
    goto nextMerAgain;

  //  And if not zero, output it, print it, and return it.

  if (_verbosity >= sayDetails) {
    fprintf(stderr, "merylOp::nextMer()-- FINISHED for operation %s with kmer %s count " F_U64 "%s\n",
            toString(_operation), _kmer.toString(kmerString), _count, ((_output != NULL) && (_count != 0)) ? " OUTPUT" : "");
    fprintf(stderr, "\n");
  }

  //  If flagged for output, output!

  if (_output != NULL) {
    _writer->addMer(_kmer, _count);
  }

  //  If flagged for printing, print!

  if (_printer != NULL) {
    char  flags[4] = { 0 };  //  Default, no flags (and no space) printed.

    if (_kmer.isCanonical()) {
      flags[0] = '\t';
      flags[1] = 'C';
    }

    if (_kmer.isPalindrome()) {
      flags[0] = '\t';
      flags[1] = 'P';
    }

    fprintf(_printer, "%s\t" F_U64 "\n", _kmer.toString(kmerString), _count);
  }

  //  Now just return and let the client query us to get the kmer and value.

  return(true);
}
