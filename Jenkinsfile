// nn-modules verify pipeline — every stage runs in the nn-build
// container via scripts/nnbuild (toolchains in the image, this
// checkout mounted at /work).  Jenkins holds no GitHub credentials:
// it builds from a local mirror the HOST keeps in sync.
pipeline {
    agent { label 'nn-dev' }
    options { timestamps(); buildDiscarder(logRotator(numToKeepStr: '30')) }
    triggers { pollSCM('H/2 * * * *') }
    stages {
        stage('host tests') {
            steps { sh 'scripts/nnbuild host-tests' }
        }
    }
}
