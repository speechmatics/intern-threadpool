from conan import ConanFile


class BoostASIOExamples(ConanFile):
    generators = "cmake_find_package"
    requires = "boost/1.79.0"

    def package_info(self):
        self.cpp_info.requires = [
            "boost::coroutine",
            "boost::system",
        ]