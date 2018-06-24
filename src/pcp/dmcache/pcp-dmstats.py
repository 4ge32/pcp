#!/usr/bin/env pmpython
# # Copyright (C) 2017 Fumiya Shigemitsu.  #
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# DmCache Software Foundation; either version 2 of the License, or (at your
# option) any later version.
#
# This program is distributed in the hope that it will be useful, but # WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
# for more details.
#
# pylint: disable=C0103,R0914,R0902,W0141
""" Display device mapper cache statistics for the system """

import re
import sys

from pcp import pmapi, pmcc
from collections import OrderedDict

if sys.version >= '3':
    long = int  # python2 to python3 portability (no long() in python3)

CACHE_METRICS = ['dmstats.histogram.hist_count',
                 'dmstats.histogram.hist_bins']

HEADING = ''
COL_HEADING = \
    ' ---------------- histogram Counts ------------------'

RATIO = True                # default to displaying cache hit ratios
BOUNDS = True                # default to displaying
REPEAT = 10                 # repeat heading after every N samples

def option(opt, optarg, index):
    """ Perform setup for an individual command line option """
    global RATIO
    global REPEAT
    if opt == 'R':
        REPEAT = int(optarg)
    elif opt == 'i': RATIO = False

def inst_dict(group, metric):
    """ Create an instance:value dictionary for the given metric """
    values = group[metric].netConvValues
    if not values:
        return {}
    return dict(map(lambda x: (x[1], x[2]), values))

def max_lv_length(group):
    """ look at the observation group and return the max length of all the lvnames """
    cache_used = inst_dict(group, 'dmstats.histogram.hist_bins')
    if not cache_used:
        return 0
    lv_names = cache_used.keys()
    return len(max(lv_names, key=len))

def make_percent(val, total):
    if (total == 0):
        return 0.0
    return (val / total)

def dm_sorted(array = []):
    sortDict = OrderedDict()
    for splitList in array:
        objList = []
        for obj in re.split("(\d+)", splitList):
            try:
                objList.append(int(obj))
            except:
                objList.append(obj)

        sortDict[splitList] = objList

    returnList = []
    for sortObjKey, sortObjValue in sorted(sortDict.items(), key=lambda x:x[1]):
        returnList.append(sortObjKey)

    return returnList

def hist_percent_bounds(group, max_lv):
    hist_count = inst_dict(group, 'dmstats.histogram.hist_count')
    res = ''
    pre_dev = ''
    pre_rg = -1
    hist_total = []
    total = 0
    index = -1
    print(dm_sorted(hist_count))
    for hist, val in hist_count.items():
        hist = hist.split(':')
        bins = hist[2]
        if bins == '0s':
            total = 0
            index = index + 1
            hist_total.append(total)
        hist_total[index] = hist_total[index] + val
    index = -1
    pre_dev = ''
    for hist, val in hist_count.items():
        hist = hist.split(':')
        dev = hist[0]
        rg = hist[1]
        bins = hist[2]
        if bins == '0s' and res != '':
            print(res)
            res = ''
            index = index + 1
        # FIX: fix static to variable where the value in rjust
        val = make_percent(val, hist_total[index])
        res = res + bins + ':' + format(str('{0:.4f}'.format(val)).rjust(6)) + ' '
        if pre_dev != dev or pre_rg != rg:
            dst = dev + ':' + rg
            print('{0}'.format(dst.center(max_lv, ' ')), end='')
            pre_dev = dev
            pre_rg = rg
    print(res)

def hist_percent_ranges(group, max_lv):
    hist_count = inst_dict(group, 'dmstats.histogram.hist_count')
    res = ''
    pre_dev = ''
    pre_rg = -1
    hist_total = []
    total = 0
    index = -1
    for hist, val in dm_sorted(hist_count.items()):
        hist = hist.split(':')
        bins = hist[2]
        if bins == '0s':
            total = 0
            index = index + 1
            hist_total.append(total)
        hist_total[index] = hist_total[index] + val
    index = -1
    pre_dev = ''
    ranges = ''
    pre_val = 0
    pre_bins = '0s'
    r = 0
    for hist, val in sorted(hist_count.items()):
        hist = hist.split(':')
        dev = hist[0]
        rg = hist[1]
        bins = hist[2]
        if bins == '0s' and res != '':
            print(res)
            res = ''
            index = index + 1
        val = make_percent(val, hist_total[index])
        if r % 2 == 1:
            res = res + ranges + ':' + format(str('{0:.4f}'.format(pre_val)).rjust(6)) + ' '
            pre_bins = bins
        else:
            pre_val = val
            ranges = pre_bins + '-' +bins
        r = r + 1
        if pre_dev != dev or pre_rg != rg:
            dst = dev + ':' + rg
            print('{0}'.format(dst.center(max_lv, ' ')), end='')
            pre_dev = dev
            pre_rg = rg
    print(res)


class DmCachePrinter(pmcc.MetricGroupPrinter):
    """ Report device mapper cache statistics """

    def __init__(self, devices):
        """ Construct object - prepare for command line handling """
        pmcc.MetricGroupPrinter.__init__(self)
        self.hostname = None
        self.devices = devices

    # it's very dirty code, I will manage it somehow...
    def report_values(self, group, width=12):
        """ Report values for one of more device mapper cache devices """

        # Build several dictionaries, keyed on cache names, with the values
        hist_dev = inst_dict(group, 'dmstats.histogram.hist_bins')
        devicelist = self.devices
        max_lv = max_lv_length(group)
        padding = ' '

        devicelist = self.devices
        if not devicelist:
            devicelist = hist_dev.keys()
        if devicelist:
            if RATIO:
                if BOUNDS:
                    hist_percent_bounds(group, max_lv)
                else:
                    hist_percent_ranges(group, max_lv)
            else:
                hist_count = inst_dict(group, 'dmstats.histogram.hist_count')
                rep = ''
                pre_dev = ''
                for hist, val in sorted(hist_count.items()):
                    hist = hist.split(':')
                    dev = hist[0]
                    reg = hist[1]
                    hist = hist[2]
                    if hist == '0s':
                        print(rep)
                        rep = ''
                    # fix static to variable where the value in rjust
                    rep = rep + hist + ':' + format(str('{0:.3f}'.format(val)).rjust(10)) + ' '
                    if pre_dev != dev:
                        dst = dev + ':' + reg
                        print('{0}'.format(dst.center(max_lv, ' ')), end='')
                        pre_dev = dev
                print(rep)
        else:
            print('No values available')

    def report(self, groups):
        """ Report driver routine - headings, sub-headings and values """
        self.convert(groups)
        group = groups['dmstats']
        max_lv = max_lv_length(group)
        padding = ""*max_lv
        if groups.counter % REPEAT == 1:
            if not self.hostname:
                self.hostname = group.contextCache.pmGetContextHostName()
            stamp = group.contextCache.pmCtime(long(group.timestamp))
            title = '@ {0} (host {1})'.format(stamp.rstrip(), self.hostname)
            if RATIO:
                style = "{0}".format(padding)
            else:
                style = "{0}".format(padding)
            HEADING = ' Name:RgID '.center(max_lv, '-') + COL_HEADING
            print('{0}\n{1}'.format(title, HEADING))
        self.report_values(group, width=max_lv)

if __name__ == '__main__':
    try:
        options = pmapi.pmOptions('iR:?')
        options.pmSetShortUsage('[options] [device ...]')
        options.pmSetOptionCallback(option)
        options.pmSetLongOptionHeader('Options')
        options.pmSetLongOption('repeat', 1, 'R', 'N', 'repeat the header after every N samples')
        options.pmSetLongOption('iops', 0, 'i', '', 'display IOPs instead of cache hit ratio')
        options.pmSetLongOptionVersion()
        options.pmSetLongOptionHelp()
        manager = pmcc.MetricGroupManager.builder(options, sys.argv)
        manager.printer = DmCachePrinter(options.pmGetOperands())
        manager['dmstats'] = CACHE_METRICS
        manager.run()
    except pmapi.pmErr as error:
        print('%s: %s\n' % (error.progname(), error.message()))
    except pmapi.pmUsageErr as usage:
        usage.message()
    except KeyboardInterrupt:
        pass
