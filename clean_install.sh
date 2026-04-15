sudo python3 -m pip uninstall fastpysgi -y

sudo rm -rf fastpysgi.egg-info/
sudo rm -f  /usr/local/lib/python3.*/dist-packages/_fastpysgi*
sudo rm -f  /usr/local/lib/python3.*/dist-packages/fastpysgi*
sudo rm -rf /usr/local/lib/python3.*/dist-packages/fastpysgi*/
sudo rm -rf /usr/lib/python3.*/site-packages/fastpysgi*/
sudo rm -rf build/ dist/ bin/ __pycache__/

if [ "$1" = "debug" ]; then
    BLD_DEBUG=1
else
    BLD_DEBUG=0
fi
# sudo CC="gcc" python3 setup.py install
sudo CC="gcc" FASTPYSGI_DEBUG=$BLD_DEBUG DISTUTILS_DEBUG=1 python3 -m pip install . -vvv --no-build-isolation

sudo ls -la /usr/local/lib/python3.*/dist-packages/_fastpysgi*

### run debug server:
# LD_PRELOAD=$(gcc -print-file-name=libasan.so) python3 server.py -g fp -v 9
