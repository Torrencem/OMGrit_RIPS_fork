# this file is intended to view the evolution of the solution over successive iterations
from scipy import *
from matplotlib import pyplot as mpl
from os import sys
#get nsteps for time from the u file

nsteps=512
mspace=32
#create the t mesh
tmesh = linspace(0,1.0,nsteps)

xmesh = linspace(0,1.0,mspace)

size = len(range(0,973,20))
print(size)
v_vec = empty([size, nsteps, mspace])
count = 0;
# for n in range(0,nsteps):
#     for x in range(0,mspace):
#         rand = random.uniform(0,.01)
#         v_vec[0,n,x] = rand
for i in range(0,973,20):
    for j in range(0,nsteps):
        sj = "%04d" % j
        si = "%04d" % i
        with open('out/advec-diff-btcs.v.out.' + si + "." + sj) as f:
            lines = f.readlines()
        split = lines[0].split(',')
        count2 = 0
        for thing in split:
            v_vec[count,j,count2] = float(split[count2])
            count2+=1
    count+=1


import numpy
import matplotlib.pyplot as plt
from mpl_toolkits.mplot3d import Axes3D

# Set up grid and test data


count=0
for i in range(0,973,20):
    hf = plt.figure(i+1)
    ha = hf.add_subplot(111, projection='3d')

    X, Y = numpy.meshgrid(xmesh, tmesh)  # `plot_surface` expects `x` and `y` data to be 2D
    ha.plot_surface(X, Y, v_vec[count])
    ha.set_xlabel('X (position)')
    ha.set_ylabel('t (time)')
    ha.set_zlabel('W value')
    ha.set_zticklabels([])
    ha.set_zticks([])
    ha.w_zaxis.line.set_lw(0.)
    count+=1

plt.show()