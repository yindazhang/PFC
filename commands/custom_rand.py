import random
class CustomRand:
	def __init__(self):
		pass
	def testCdf(self, cdf):
		if cdf[0][1] != 0.0:
			return False
		if cdf[-1][1] != 1.0:
			return False
		for i in range(1, len(cdf)):
			if cdf[i][1] <= cdf[i-1][1] or cdf[i][0] <= cdf[i-1][0]:
				return False
		return True
	def setCdf(self, cdf):
		if not self.testCdf(cdf):
			return False
		self.cdf = cdf
		return True
	def getAvg(self):
		s = 0
		last_x, last_y = self.cdf[0]
		for c in self.cdf[1:]:
			x, y = c
			s += (x + last_x)/2.0 * (y - last_y)
			last_x = x
			last_y = y
		return s
	def rand(self):
		r = random.random()
		low = 0
		high = len(self.cdf) - 1

		while low < high:
			mid = (low + high) // 2
			if self.cdf[mid][1] > r:
				high = mid
			else:
				low = mid + 1
		
		#print(low)
		x0,y0 = self.cdf[low-1]
		x1,y1 = self.cdf[low]
		return x0 + (x1-x0)/(y1-y0)*(r-y0)