# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

from __future__ import absolute_import, print_function, unicode_literals

from copy import deepcopy

from voluptuous import (
    Any,
    Optional,
    Required,
    Extra,
)

from taskgraph.transforms.base import TransformSequence
from taskgraph.transforms.tests import test_description_schema
from taskgraph.util.schema import optionally_keyed_by, resolve_keyed_by, Schema
from taskgraph.util.treeherder import split_symbol, join_symbol

transforms = TransformSequence()


raptor_description_schema = Schema({
    # Raptor specific configs.
    Optional('apps'): optionally_keyed_by(
        'test-platform',
        [basestring]
    ),
    Optional('raptor-test'): basestring,
    Optional('raptor-subtests'): [basestring],
    Optional('activity'): optionally_keyed_by(
        'app',
        basestring
    ),
    Optional('binary-path'): optionally_keyed_by(
        'app',
        basestring
    ),
    Optional('pageload'): optionally_keyed_by(
        'test-platform', 'app',
        Any('cold', 'warm', 'both'),
    ),
    Optional('condprof'): optionally_keyed_by(
        'app',
        bool,
    ),
    # Configs defined in the 'test_description_schema'.
    Optional('max-run-time'): optionally_keyed_by(
        'app',
        test_description_schema['max-run-time']
    ),
    Optional('run-on-projects'): optionally_keyed_by(
        'app',
        test_description_schema['run-on-projects']
    ),
    Optional('variants'): optionally_keyed_by(
        'app',
        test_description_schema['variants']
    ),
    Optional('target'): optionally_keyed_by(
        'app',
        test_description_schema['target']
    ),
    Optional('run-visual-metrics'): optionally_keyed_by(
        'app',
        bool
    ),
    Required('test-name'): test_description_schema['test-name'],
    Required('test-platform'): test_description_schema['test-platform'],
    Required('require-signed-extensions'): test_description_schema['require-signed-extensions'],
    Required('treeherder-symbol'): test_description_schema['treeherder-symbol'],
    # Any unrecognized keys will be validated against the test_description_schema.
    Extra: object,
})

transforms.add_validate(raptor_description_schema)


@transforms.add
def set_defaults(config, tests):
    for test in tests:
        test.setdefault('pageload', 'warm')
        test.setdefault('run-visual-metrics', False)
        yield test


@transforms.add
def split_apps(config, tests):
    app_symbols = {
        'chrome': 'ChR',
        'chromium': 'Cr',
        'fenix': 'fenix',
        'refbrow': 'refbrow',
    }

    for test in tests:
        apps = test.pop('apps', None)
        if not apps:
            yield test
            continue

        for app in apps:
            atest = deepcopy(test)
            suffix = "-{}".format(app)
            atest['app'] = app
            atest['description'] += " on {}".format(app.capitalize())

            name = atest['test-name']
            if name.endswith('-cold'):
                name = atest['test-name'][:-len('-cold')] + suffix + '-cold'
            else:
                name += suffix

            atest['test-name'] = name
            atest['try-name'] = name

            if app in app_symbols:
                group, symbol = split_symbol(atest['treeherder-symbol'])
                group += "-{}".format(app_symbols[app])
                atest['treeherder-symbol'] = join_symbol(group, symbol)

            yield atest


@transforms.add
def handle_keyed_by_app(config, tests):
    fields = [
        'condprof',
        'variants',
        'limit-platforms',
        'activity',
        'binary-path',
        'pageload',
        'max-run-time',
        'run-on-projects',
        'target',
        'run-visual-metrics'
    ]
    for test in tests:
        for field in fields:
            resolve_keyed_by(test, field, item_name=test['test-name'])
        yield test


@transforms.add
def split_pageload(config, tests):
    for test in tests:
        pageload = test.pop('pageload', 'warm')

        if pageload not in ('cold', 'both'):
            yield test
            continue

        if pageload == 'both':
            orig = deepcopy(test)
            yield orig

        assert 'raptor-test' in test
        test['description'] += " using cold pageload"

        # for raptor-webext to run cold we just call the corresponding '-cold' test name; but
        # for raptor browsertime we leave the raptor test name as/is and will set the '--cold'
        # command line argument instead via settting test['cold'] to true
        if test['test-name'].startswith('browsertime-tp6'):
            test['cold'] = True
        else:
            test['raptor-test'] += '-cold'

        test['max-run-time'] = 3000
        test['test-name'] += '-cold'
        test['try-name'] += '-cold'

        group, symbol = split_symbol(test['treeherder-symbol'])
        symbol += '-c'
        test['treeherder-symbol'] = join_symbol(group, symbol)
        yield test


@transforms.add
def build_condprof_tests(config, tests):
    for test in tests:
        if not test.pop('condprof', False):
            yield test
            continue
        if 'chrome' in test['test-name'] or 'chromium' in test['test-name']:
            yield test
            continue
        if test['test-platform'].startswith('windows10-aarch64'):
            yield test
            continue

        # Make condprof test
        condprof_test = deepcopy(test)
        yield test

        extra_options = condprof_test.setdefault('mozharness', {}).setdefault('extra-options', [])
        extra_options.append('--with-conditioned-profile')

        group, symbol = split_symbol(condprof_test['treeherder-symbol'])
        symbol += '-condprof'

        condprof_test['description'] += " with condprof"
        condprof_test['try-name'] += '-condprof'
        condprof_test['test-name'] += '-condprof'
        condprof_test['treeherder-symbol'] = join_symbol(group, symbol)

        yield condprof_test


@transforms.add
def split_browsertime_page_load_by_url(config, tests):

    for test in tests:

        # for tests that have 'raptor-subtests' listed, we want to create a separate
        # test job for every subtest (i.e. split out each page-load URL into its own job)
        subtests = test.pop('raptor-subtests', None)
        if not subtests:
            yield test
            continue

        chunk_number = 0

        for subtest in subtests:

            # create new test job
            chunked = deepcopy(test)

            # only run the subtest/single URL
            chunked['test-name'] += "-{}".format(subtest)
            chunked['try-name'] += "-{}".format(subtest)
            chunked['raptor-test'] = subtest

            # set treeherder symbol and description
            chunk_number += 1
            group, symbol = split_symbol(test['treeherder-symbol'])
            symbol += "-{}".format(chunk_number)
            chunked['treeherder-symbol'] = join_symbol(group, symbol)
            chunked['description'] += "-{}".format(subtest)

            yield chunked


@transforms.add
def add_extra_options(config, tests):
    for test in tests:
        extra_options = test.setdefault('mozharness', {}).setdefault('extra-options', [])

        if test.pop('run-visual-metrics', False):
            extra_options.append('--browsertime-video')
            test['attributes']['run-visual-metrics'] = True

        if 'app' in test:
            extra_options.append('--app={}'.format(test.pop('app')))

        # for browsertime tp6 cold page-load jobs we need to set the '--cold' cmd line arg
        if test['test-name'].startswith('browsertime-tp6'):
            if test.pop('cold', False) is True:
                extra_options.append('--cold')

        if 'activity' in test:
            extra_options.append('--activity={}'.format(test.pop('activity')))

        if 'binary-path' in test:
            extra_options.append('--binary-path={}'.format(test.pop('binary-path')))

        if 'raptor-test' in test:
            extra_options.append('--test={}'.format(test.pop('raptor-test')))

        if test['require-signed-extensions']:
            extra_options.append('--is-release-build')

        # add urlparams based on platform, test names and projects
        testurlparams_by_platform_and_project = {
            "android-hw-g5": [
                {
                    "branches": [],  # For all branches
                    "testnames": ["youtube-playback"],
                    "urlparams": [
                        # param used for excluding youtube-playback tests from executing
                        # it excludes the tests with videos >1080p
                        "exclude=1,2,9,10,17,18,21,22,26,28,30,32,39,40,47,"
                        "48,55,56,63,64,71,72,79,80,83,84,89,90,95,96",
                    ]
                },
            ]
        }

        for platform, testurlparams_by_project_definitions \
                in testurlparams_by_platform_and_project.items():

            if test['test-platform'].startswith(platform):
                # For every platform it may have several definitions
                for testurlparams_by_project in testurlparams_by_project_definitions:
                    # The test should contain at least one defined testname
                    if any(
                        testname in test['test-name']
                        for testname in testurlparams_by_project['testnames']
                    ):
                        branches = testurlparams_by_project['branches']
                        if (
                            branches == [] or
                            config.params.get('project') in branches or
                            config.params.is_try() and 'try' in branches
                        ):
                            params_query = '&'.join(testurlparams_by_project['urlparams'])
                            add_extra_params_option = "--test-url-params={}".format(params_query)
                            extra_options.append(add_extra_params_option)

        yield test
