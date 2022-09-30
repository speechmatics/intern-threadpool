# Copyright 2022 Cantab Research Ltd.
# Licensed under the MIT license. See LICENSE.txt in the project root for details.

from conan import ConanFile


class BoostASIOExamples(ConanFile):
    generators = "cmake_find_package"
    requires = "boost/1.79.0"

    def package_info(self):
        self.cpp_info.requires = [
            "boost::coroutine",
            "boost::system",
        ]
