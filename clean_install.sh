sudo python3 -m pip uninstall fastpysgi -y

sudo rm -f  /usr/local/lib/python3.*/dist-packages/_fastpysgi*
sudo rm -f  /usr/local/lib/python3.*/dist-packages/fastpysgi*
sudo rm -rf /usr/local/lib/python3.*/dist-packages/fastpysgi*/
sudo rm -rf /usr/lib/python3.*/site-packages/fastpysgi*/
sudo rm -rf build/ dist/ bin/ __pycache__/

# sudo CC="gcc" python3 setup.py install
sudo CC="gcc" DISTUTILS_DEBUG=1 python3 -m pip install . -vvv --no-build-isolation

sudo ls -la /usr/local/lib/python3.*/dist-packages/_fastpysgi*
