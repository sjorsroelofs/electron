import { expect } from 'chai'
import * as cp from 'child_process'
import * as http from 'http'
import * as express from 'express'
import * as fs from 'fs-extra'
import * as os from 'os'
import * as path from 'path'
import { AddressInfo } from 'net';

const features = process.electronBinding('features')

const fixturesPath = path.resolve(__dirname, '../spec/fixtures')

// We can only test the auto updater on darwin non-component builds
const describeFn = (process.platform === 'darwin' && !process.mas && !features.isComponentBuild() ? describe : describe.skip)

describeFn('autoUpdater behavior', function () {
  this.timeout(120000)

  let identity = ''

  beforeEach(function () {
    const result = cp.spawnSync(path.resolve(__dirname, '../script/codesign/get-trusted-identity.sh'))
    if (result.status !== 0 || result.stdout.toString().trim().length === 0)  {
      // Per https://circleci.com/docs/2.0/env-vars:
      // CIRCLE_PR_NUMBER is only present on forked PRs
      if (process.env.CI && !process.env.CIRCLE_PR_NUMBER) {
        throw new Error('No valid signing identity available to run autoUpdater specs')
      }
      this.skip()
    } else {
      identity = result.stdout.toString().trim()
    }
  })

  it('should have a valid code signing identity', () => {
    expect(identity).to.be.a('string').with.lengthOf.at.least(1)
  })

  const copyApp = async (newDir: string, fixture = 'initial') => {
    const appBundlePath = path.resolve(process.execPath, '../../..')
    const newPath = path.resolve(newDir, 'Electron.app')
    cp.spawnSync('cp', ['-R', appBundlePath, path.dirname(newPath)])
    const appDir = path.resolve(newPath, 'Contents/Resources/app')
    await fs.mkdirp(appDir)
    await fs.copy(path.resolve(fixturesPath, 'auto-update', fixture), appDir)
    const plistPath = path.resolve(newPath, 'Contents', 'Info.plist')
    await fs.writeFile(
      plistPath,
      (await fs.readFile(plistPath, 'utf8')).replace('<key>BuildMachineOSBuild</key>', `<key>NSAppTransportSecurity</key>
      <dict>
          <key>NSAllowsArbitraryLoads</key>
          <true/>
          <key>NSExceptionDomains</key>
          <dict>
              <key>localhost</key>
              <dict>
                  <key>NSExceptionAllowsInsecureHTTPLoads</key>
                  <true/>
                  <key>NSIncludesSubdomains</key>
                  <true/>
              </dict>
          </dict>
      </dict><key>BuildMachineOSBuild</key>`)
    )
    return newPath
  }

  const spawn = (cmd: string, args: string[], opts: any = {}) => {
    let out = ''
    const child = cp.spawn(cmd, args, opts)
    child.stdout.on('data', (chunk: Buffer) => {
      out += chunk.toString()
    })
    child.stderr.on('data', (chunk: Buffer) => {
      out += chunk.toString()
    })
    return new Promise<{ code: number, out: string }>((resolve) => {
      child.on('exit', (code, signal) => {
        expect(signal).to.equal(null)
        resolve({
          code: code!,
          out
        })
      })
    })
  }

  const signApp = (appPath: string) => {
    return spawn('codesign', ['-s', identity, '--deep', '--force', appPath])
  }

  const launchApp = (appPath: string, args: string[] = []) => {
    return spawn(path.resolve(appPath, 'Contents/MacOS/Electron'), args)
  }

  const withTempDirectory = async (fn: (dir: string) => Promise<void>) => {
    const dir = await fs.mkdtemp(path.resolve(os.tmpdir(), 'electron-update-spec-'))
    try {
      await fn(dir)
    } finally {
      cp.spawnSync('rm', ['-r' , dir])
    }
  }

  const logOnError = (what: any, fn: () => void) => {
    try {
      fn()
    } catch (err) {
      console.error(what)
      throw err
    }
  }

  it('should fail to set the feed URL when the app is not signed', async () => {
    await withTempDirectory(async (dir) => {
      const appPath = await copyApp(dir)
      const launchResult = await launchApp(appPath, ['http://myupdate'])
      expect(launchResult.code).to.equal(1)
      expect(launchResult.out).to.include('Could not get code signature for running application')
    })
  })

  it('should cleanly set the feed URL when the app is signed', async () => {
    await withTempDirectory(async (dir) => {
      const appPath = await copyApp(dir)
      await signApp(appPath)
      const launchResult = await launchApp(appPath, ['http://myupdate'])
      expect(launchResult.code).to.equal(0)
      expect(launchResult.out).to.include('Feed URL Set: http://myupdate')
    })
  })

  describe('with update server', () => {
    let port = 0;
    let server: express.Application = null as any;
    let httpServer: http.Server = null as any
    let requests: express.Request[] = [];

    beforeEach((done) => {
      requests = []
      server = express()
      server.use((req, res, next) => {
        requests.push(req)
        next()
      })
      httpServer = server.listen(0, '127.0.0.1', () => {
        port = (httpServer.address() as AddressInfo).port
        done()
      })
    })

    afterEach((done) => {
      if (httpServer) {
        httpServer.close(() => {
          httpServer = null as any
          server = null as any
          done()
        })
      }
    })

    it('should hit the update endpoint when checkForUpdates is called', async () => {
      await withTempDirectory(async (dir) => {
        const appPath = await copyApp(dir, 'check')
        await signApp(appPath)
        server.get('/update-check', (req, res) => {
          res.status(204).send()
        })
        const launchResult = await launchApp(appPath, [`http://localhost:${port}/update-check`])
        logOnError(launchResult, () => {
          expect(launchResult.code).to.equal(0)
          expect(requests).to.have.lengthOf(1)
          expect(requests[0]).to.have.property('url', '/update-check')
          expect(requests[0].header('user-agent')).to.include('Electron/')
        })
      })
    })

    it('should hit the download endpoint when an update is available and error if the file is bad', async () => {
      await withTempDirectory(async (dir) => {
        const appPath = await copyApp(dir, 'update')
        await signApp(appPath)
        server.get('/update-file', (req, res) => {
          res.status(500).send('This is not a file')
        })
        server.get('/update-check', (req, res) => {
          res.json({
            url: `http://localhost:${port}/update-file`,
            name: 'My Release Name',
            notes: 'Theses are some release notes innit',
            pub_date: (new Date()).toString()
          })
        })
        const launchResult = await launchApp(appPath, [`http://localhost:${port}/update-check`])
        logOnError(launchResult, () => {
          expect(launchResult).to.have.property('code', 1)
          expect(launchResult.out).to.include('Update download failed. The server sent an invalid response.')
          expect(requests).to.have.lengthOf(2)
          expect(requests[0]).to.have.property('url', '/update-check')
          expect(requests[1]).to.have.property('url', '/update-file')
          expect(requests[0].header('user-agent')).to.include('Electron/')
          expect(requests[1].header('user-agent')).to.include('Electron/')
        })
      })
    })

    it('should hit the download endpoint when an update is available and update successfully when the zip is provided', async () => {
      await withTempDirectory(async (dir) => {
        const appPath = await copyApp(dir, 'update')
        await signApp(appPath)

        // Prepare update
        await withTempDirectory(async (dir2) => {
          const secondAppPath = await copyApp(dir2, 'update')
          const appPJPath = path.resolve(secondAppPath, 'Contents', 'Resources', 'app', 'package.json')
          await fs.writeFile(
            appPJPath,
            (await fs.readFile(appPJPath, 'utf8')).replace('1.0.0', '2.0.0')
          )
          await signApp(secondAppPath)
          const updateZipPath = path.resolve(dir2, 'update.zip');
          await spawn('zip', ['-r', '--symlinks', updateZipPath, './'], {
            cwd: dir2
          })

          server.get('/update-file', (req, res) => {
            res.download(updateZipPath)
          })
          server.get('/update-check', (req, res) => {
            res.json({
              url: `http://localhost:${port}/update-file`,
              name: 'My Release Name',
              notes: 'Theses are some release notes innit',
              pub_date: (new Date()).toString()
            })
          })
          const relaunchPromise = new Promise((resolve, reject) => {
            server.get('/update-check/updated/:version', (req, res) => {
              res.status(204).send()
              resolve()
            })
          })
          const launchResult = await launchApp(appPath, [`http://localhost:${port}/update-check`])
          logOnError(launchResult, () => {
            expect(launchResult).to.have.property('code', 0)
            expect(launchResult.out).to.include('Update Downloaded')
            expect(requests).to.have.lengthOf(2)
            expect(requests[0]).to.have.property('url', '/update-check')
            expect(requests[1]).to.have.property('url', '/update-file')
            expect(requests[0].header('user-agent')).to.include('Electron/')
            expect(requests[1].header('user-agent')).to.include('Electron/')
          })

          await relaunchPromise
          expect(requests).to.have.lengthOf(3)
          expect(requests[2]).to.have.property('url', '/update-check/updated/2.0.0')
          expect(requests[2].header('user-agent')).to.include('Electron/')
        })
      })
    })
  })
})
